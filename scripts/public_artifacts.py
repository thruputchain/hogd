"""Create-only S3 publication with verified anonymous reads."""
from __future__ import annotations

import hashlib
import re
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Callable

PUBLIC_READ_TIMEOUT_SECONDS = 60
MAX_CONDITIONAL_UPLOAD_ATTEMPTS = 3

class PublicationError(Exception):
    """Invalid artifacts or failed immutable publication."""


def sha256_file(path: Path) -> str:
    """Return the lowercase SHA-256 of a file."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def public_url(base_url: str, key: str) -> str:
    """Return the public HTTPS URL for an object key."""
    if not base_url.startswith("https://"):
        raise PublicationError("public base URL must use HTTPS")
    encoded = urllib.parse.quote(key, safe="/")
    return f"{base_url.rstrip('/')}/{encoded}"


def verify_public_object(
    url: str,
    expected_size: int,
    expected_sha256: str,
    *,
    opener: Callable = urllib.request.urlopen,
) -> None:
    """Download a public object with a bound and verify its exact bytes."""
    digest = hashlib.sha256()
    received = 0
    try:
        with opener(url, timeout=PUBLIC_READ_TIMEOUT_SECONDS) as response:
            content_length = response.headers.get("Content-Length")
            if content_length is not None:
                try:
                    parsed_content_length = int(content_length)
                except ValueError as error:
                    raise PublicationError(
                        f"invalid public Content-Length for {url}: {content_length!r}"
                    ) from error
                if parsed_content_length != expected_size:
                    raise PublicationError(
                        f"public Content-Length mismatch for {url}: "
                        f"{content_length} != {expected_size}"
                    )
            while True:
                chunk = response.read(min(1024 * 1024, expected_size + 1 - received))
                if not chunk:
                    break
                received += len(chunk)
                if received > expected_size:
                    raise PublicationError(f"public object exceeds expected size: {url}")
                digest.update(chunk)
    except (OSError, urllib.error.URLError) as error:
        raise PublicationError(f"could not read public object {url}: {error}") from error

    if received != expected_size:
        raise PublicationError(
            f"public object size mismatch for {url}: {received} != {expected_size}"
        )
    actual_sha256 = digest.hexdigest()
    if actual_sha256 != expected_sha256:
        raise PublicationError(
            f"public object SHA-256 mismatch for {url}: "
            f"{actual_sha256} != {expected_sha256}"
        )


def command_error_detail(result: subprocess.CompletedProcess) -> str:
    """Return a stable diagnostic from a failed subprocess."""
    return (
        (result.stderr or "").strip()
        or (result.stdout or "").strip()
        or "unknown error"
    )


def classify_conditional_write_failure(
    result: subprocess.CompletedProcess,
) -> str | None:
    """Classify only the S3 conditional-write failures we handle specially."""
    detail = command_error_detail(result)
    if re.search(r"\bPreconditionFailed\b|\b412 Precondition Failed\b", detail):
        return "precondition"
    if re.search(
        r"\bConditionalRequestConflict\b|\b409 (?:Conflict|Conditional Request Conflict)\b",
        detail,
    ):
        return "conflict"
    return None


def publish_file(
    item: dict,
    *,
    bucket: str,
    base_url: str,
    runner: Callable = subprocess.run,
    opener: Callable = urllib.request.urlopen,
    sleeper: Callable = time.sleep,
) -> str:
    """Create one immutable object or verify an identical existing object."""
    command = [
        "aws",
        "s3api",
        "put-object",
        "--bucket",
        bucket,
        "--key",
        item["key"],
        "--body",
        str(item["path"]),
        "--content-type",
        item["content_type"],
        "--cache-control",
        "public,max-age=31536000,immutable",
        "--server-side-encryption",
        "AES256",
        "--checksum-algorithm",
        "SHA256",
        "--if-none-match",
        "*",
        "--metadata",
        f"sha256={item['sha256']}",
    ]
    url = public_url(base_url, item["key"])

    for attempt in range(1, MAX_CONDITIONAL_UPLOAD_ATTEMPTS + 1):
        result = runner(command, capture_output=True, text=True)
        if result.returncode == 0:
            break

        failure = classify_conditional_write_failure(result)
        if failure == "conflict" and attempt < MAX_CONDITIONAL_UPLOAD_ATTEMPTS:
            sleeper(2 ** (attempt - 1))
            continue
        if failure == "precondition":
            try:
                verify_public_object(
                    url,
                    item["size"],
                    item["sha256"],
                    opener=opener,
                )
            except PublicationError as verification_error:
                raise PublicationError(
                    f"create-only upload found an existing object at "
                    f"s3://{bucket}/{item['key']}, but verification failed: "
                    f"{verification_error}"
                ) from verification_error
            return "existing"

        detail = command_error_detail(result)
        if failure == "conflict":
            raise PublicationError(
                f"conditional upload conflict for s3://{bucket}/{item['key']} "
                f"after {attempt} attempts: {detail}"
            )
        raise PublicationError(
            f"upload failed for s3://{bucket}/{item['key']}: {detail}"
        )

    verify_public_object(
        url,
        item["size"],
        item["sha256"],
        opener=opener,
    )
    return "created"
