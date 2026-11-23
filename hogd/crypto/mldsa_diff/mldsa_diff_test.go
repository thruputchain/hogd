// Package mldsadiff checks that hogd's ML-DSA-65 agrees with Go's
// crypto/mldsa, an independent implementation of FIPS 204.
//
// TestDifferential runs mldsa_oracle (../mldsa_oracle.cc, a target of hogd's
// CMake build) and, for random seeds, messages and context strings, checks
// that:
//
//   - both derive the same public key from a seed;
//   - both make the same deterministic signature, byte for byte;
//   - both accept those signatures, and hogd's signatures made with random
//     signing randomness;
//   - both reject corrupted signatures, public keys, messages and context
//     strings, including signatures whose hint is encoded wrongly but still
//     means the same thing, which only a strict decoder rejects.
//
// hogd only verifies signatures in production, so what matters most is that
// the two verifiers agree. Run the test from this directory with
//
//	go test -args -oracle=/path/to/build/hogd/crypto/mldsa_oracle
//
// A run makes its random choices from a seed, which it logs; -seed=N repeats
// a run exactly. -keys sets how many keys to test.
package mldsadiff

import (
	"bytes"
	"crypto/mldsa"
	"encoding/binary"
	"encoding/hex"
	"flag"
	"fmt"
	"math/rand/v2"
	"testing"
)

var (
	oraclePath = flag.String("oracle", "", "path to mldsa_oracle, from hogd's build")
	numKeys    = flag.Int("keys", 50, "number of random keys to test")
	runSeed    = flag.Uint64("seed", 0, "seed for the test's random choices (default: a random seed)")
)

const (
	pkSize  = mldsa.MLDSA65PublicKeySize
	sigSize = mldsa.MLDSA65SignatureSize

	// maxContext is the length of the longest context string ML-DSA accepts.
	maxContext = 255

	// An ML-DSA-65 signature is c~ (48 bytes), then z (5 polynomials of 256
	// 20-bit coefficients), then the hint (FIPS 204, Algorithm 26). The hint
	// lists the indices of the coefficients it marks in each of k = 6
	// polynomials, increasing within each polynomial, in omega = 55 bytes
	// padded with zeros, followed by k cumulative counts (Algorithm 20).
	cTildeSize = 48
	hintStart  = cTildeSize + 5*256*20/8
	omega      = 55
	k          = 6

	messagesPerKey = 3
	maxFailures    = 10
)

// The signature layout above covers the whole signature.
var _ = [1]struct{}{}[sigSize-(hintStart+omega+k)]

func TestDifferential(t *testing.T) {
	if *oraclePath == "" {
		t.Fatal("-oracle is not set; see the package documentation")
	}
	o, err := startOracle(*oraclePath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := o.close(); err != nil {
			t.Errorf("mldsa_oracle: %v", err)
		}
	})

	seed := *runSeed
	if seed == 0 {
		seed = rand.Uint64()
	}
	t.Logf("seed %d (-seed=%d repeats this run)", seed, seed)
	var chachaSeed [32]byte
	binary.LittleEndian.PutUint64(chachaSeed[:], seed)
	src := rand.NewChaCha8(chachaSeed)
	d := &differ{t: t, oracle: o, src: src, rng: rand.New(src)}

	var pk []byte
	for i := range *numKeys {
		pk = d.testKey(i, pk)
	}
	t.Logf("compared %d keys, %d signatures and %d verifications",
		*numKeys, d.signatures, d.verifications)
}

// differ makes the random choices of a test run and compares hogd with Go.
type differ struct {
	t      *testing.T
	oracle *oracle
	src    *rand.ChaCha8 // random bytes
	rng    *rand.Rand    // random numbers, from src

	failures, signatures, verifications int
}

// testKey compares key generation from a random seed, then signing and
// verification with the key. otherPK is another key's public key, or nil. It
// returns the key's public key.
func (d *differ) testKey(i int, otherPK []byte) []byte {
	seed := d.bytes(mldsa.PrivateKeySize)
	where := fmt.Sprintf("key %d (seed %x)", i, seed)
	sk, err := mldsa.NewPrivateKey(mldsa.MLDSA65(), seed)
	if err != nil {
		d.t.Fatalf("%s: crypto/mldsa: %v", where, err)
	}
	pk := sk.PublicKey().Bytes()
	hogdPK, err := d.oracle.publicKey(seed)
	if err != nil {
		d.t.Fatalf("%s: %v", where, err)
	}
	if !bytes.Equal(pk, hogdPK) {
		d.fail(where, "public keys differ\nGo:   %x\nhogd: %x", pk, hogdPK)
		return pk
	}
	for j := range messagesPerKey {
		d.testMessage(fmt.Sprintf("%s, message %d", where, j), sk, pk, otherPK)
	}
	return pk
}

// testMessage compares signing a random message under a random context
// string, and verification of the signature and of corruptions of it.
func (d *differ) testMessage(where string, sk *mldsa.PrivateKey, pk, otherPK []byte) {
	msg, ctx := d.message(), d.context()
	where = fmt.Sprintf("%s %s, context %s", where, describe(msg), describe(ctx))

	// Deterministic signatures: an all-zero signing randomness for hogd.
	sig, goErr := sk.SignDeterministic(msg, &mldsa.Options{Context: string(ctx)})
	hogdSig := d.sign(where, make([]byte, 32), ctx, msg)
	d.signatures++
	wantSigned := len(ctx) <= maxContext
	if (goErr == nil) != wantSigned || (hogdSig != nil) != wantSigned {
		d.fail(where, "deterministic signing: Go %s, hogd %s",
			signedOrNot(goErr == nil), signedOrNot(hogdSig != nil))
		return
	}
	if !wantSigned {
		// Nor may either accept a signature under an over-long context.
		sig, err := sk.SignDeterministic(msg, &mldsa.Options{Context: string(ctx[:maxContext])})
		if err != nil {
			d.t.Fatalf("%s: crypto/mldsa: %v", where, err)
		}
		d.verify(where+", signed under its first 255 bytes", pk, sig, ctx, msg, false)
		return
	}
	if i := firstDifference(sig, hogdSig); i >= 0 {
		d.fail(where, "deterministic signatures differ, first at byte %d (in %s)", i, region(i))
	}
	d.verify(where, pk, sig, ctx, msg, true)

	// hogd's signature with random signing randomness.
	rnd := d.bytes(32)
	if hedged := d.sign(where, rnd, ctx, msg); hedged == nil {
		d.fail(where, "hogd refuses to sign with rnd %x", rnd)
	} else {
		d.verify(fmt.Sprintf("%s, hogd's signature with rnd %x", where, rnd),
			pk, hedged, ctx, msg, true)
	}

	for _, corrupt := range corruptions {
		in := input{pk: pk, sig: sig, ctx: ctx, msg: msg}
		if what := corrupt(d, &in); what != "" {
			d.verify(where+", "+what, in.pk, in.sig, in.ctx, in.msg, false)
		}
	}
	if otherPK != nil {
		d.verify(where+", another key's public key", otherPK, sig, ctx, msg, false)
	}
}

// sign has hogd sign msg with the current key, returning nil if it refuses.
func (d *differ) sign(where string, rnd, ctx, msg []byte) []byte {
	sig, err := d.oracle.sign(rnd, ctx, msg)
	if err != nil {
		d.t.Fatalf("%s: %v", where, err)
	}
	return sig
}

// verify checks that hogd and Go both give the verdict want.
func (d *differ) verify(where string, pk, sig, ctx, msg []byte, want bool) {
	d.verifications++
	hogd, err := d.oracle.verify(pk, sig, ctx, msg)
	if err != nil {
		d.t.Fatalf("%s: %v", where, err)
	}
	goAccepts := goVerify(pk, sig, ctx, msg)
	switch {
	case hogd != goAccepts:
		d.fail(where, "verifiers disagree: hogd %s, Go %s", verdict(hogd), verdict(goAccepts))
	case hogd && !want:
		d.fail(where, "hogd and Go both accept")
	case !hogd && want:
		d.fail(where, "hogd and Go both reject")
	}
}

func goVerify(pk, sig, ctx, msg []byte) bool {
	key, err := mldsa.NewPublicKey(mldsa.MLDSA65(), pk)
	if err != nil {
		return false
	}
	return mldsa.Verify(key, msg, sig, &mldsa.Options{Context: string(ctx)}) == nil
}

func (d *differ) fail(where, format string, args ...any) {
	d.t.Helper()
	d.t.Errorf("%s: %s", where, fmt.Sprintf(format, args...))
	d.failures++
	if d.failures >= maxFailures {
		d.t.Fatalf("stopping after %d failures", d.failures)
	}
}

func (d *differ) bytes(n int) []byte {
	b := make([]byte, n)
	d.src.Read(b)
	return b
}

// message returns a random message: usually some hundreds of bytes, now and
// then empty or tens of kilobytes.
func (d *differ) message() []byte {
	var n int
	switch r := d.rng.IntN(16); {
	case r == 0:
		n = 0
	case r < 4:
		n = 1 + d.rng.IntN(64)
	case r < 15:
		n = 65 + d.rng.IntN(2048)
	default:
		n = d.rng.IntN(1 << 16)
	}
	return d.bytes(n)
}

// context returns a random context string: often empty or of the maximum
// length, and now and then over-long.
func (d *differ) context() []byte {
	var n int
	switch r := d.rng.IntN(16); {
	case r < 4:
		n = 0
	case r < 13:
		n = 1 + d.rng.IntN(maxContext-1)
	case r < 15:
		n = maxContext
	default:
		n = maxContext + 1 + d.rng.IntN(45)
	}
	return d.bytes(n)
}

// describe abbreviates a message or context string for failure messages;
// the run's seed reproduces it exactly.
func describe(b []byte) string {
	switch {
	case len(b) == 0:
		return "(empty)"
	case len(b) <= 32:
		return hex.EncodeToString(b)
	default:
		return fmt.Sprintf("%x... (%d bytes)", b[:16], len(b))
	}
}

func firstDifference(a, b []byte) int {
	for i := range a {
		if a[i] != b[i] {
			return i
		}
	}
	return -1
}

// region names the part of a signature byte i is in.
func region(i int) string {
	switch {
	case i < cTildeSize:
		return "c~"
	case i < hintStart:
		return "z"
	default:
		return "the hint"
	}
}

func verdict(accepts bool) string {
	if accepts {
		return "accepts"
	}
	return "rejects"
}

func signedOrNot(signed bool) string {
	if signed {
		return "signs"
	}
	return "refuses"
}

// input is what verification takes.
type input struct{ pk, sig, ctx, msg []byte }

// A corruption changes a valid input so that verification must reject it,
// and describes the change. It returns "" if it does not apply to the input.
// It must copy a slice before changing its contents.
type corruption func(d *differ, in *input) string

var corruptions = []corruption{
	flipSignatureBit(0, sigSize),
	flipSignatureBit(0, cTildeSize),
	flipSignatureBit(cTildeSize, hintStart),
	flipSignatureBit(hintStart, sigSize),
	changeSignatureByte,
	randomSignature,
	randomCTildeAndZ,

	// Hints encoded wrongly, though unsortHints and duplicateHint, and
	// padHintsWithNonzero if the padding is ignored, leave what the hint
	// means unchanged.
	unsortHints,
	duplicateHint,
	padHintsWithNonzero,
	overflowHintCount,
	decreaseHintCount,

	// Well-formed hints that mean something else.
	dropHint,
	addHint,

	flipPublicKeyBit,
	flipMessageBit,
	appendToMessage,
	truncateMessage,
	flipContextBit,
	appendToContext,
	truncateContext,
	overlongContext,
}

// flipBit copies *b and flips a random bit in bytes [lo, hi) of the copy,
// returning the bit's index.
func (d *differ) flipBit(b *[]byte, lo, hi int) int {
	*b = bytes.Clone(*b)
	bit := 8*lo + d.rng.IntN(8*(hi-lo))
	(*b)[bit/8] ^= 1 << (bit % 8)
	return bit
}

// flipSignatureBit flips a random bit in bytes [lo, hi) of the signature.
func flipSignatureBit(lo, hi int) corruption {
	return func(d *differ, in *input) string {
		bit := d.flipBit(&in.sig, lo, hi)
		return fmt.Sprintf("signature bit %d (in %s) flipped", bit, region(bit/8))
	}
}

func changeSignatureByte(d *differ, in *input) string {
	in.sig = bytes.Clone(in.sig)
	i := d.rng.IntN(sigSize)
	old := in.sig[i]
	in.sig[i] ^= byte(1 + d.rng.IntN(255))
	return fmt.Sprintf("signature byte %d (in %s) changed from %d to %d", i, region(i), old, in.sig[i])
}

func randomSignature(d *differ, in *input) string {
	in.sig = d.bytes(sigSize)
	return "random signature"
}

func randomCTildeAndZ(d *differ, in *input) string {
	in.sig = append(d.bytes(hintStart), in.sig[hintStart:]...)
	return "signature's c~ and z random"
}

// hint returns the hint part of sig: the coefficient indices, and the
// cumulative counts of indices for each polynomial. Polynomial p's indices
// are idx[polyStart(counts, p):counts[p]].
func hint(sig []byte) (idx, counts []byte) {
	return sig[hintStart : hintStart+omega], sig[hintStart+omega:]
}

func polyStart(counts []byte, p int) int {
	if p == 0 {
		return 0
	}
	return int(counts[p-1])
}

// unsortHints swaps two adjacent indices of a polynomial.
func unsortHints(d *differ, in *input) string {
	_, counts := hint(in.sig)
	var polys []int
	for p := range k {
		if int(counts[p])-polyStart(counts, p) >= 2 {
			polys = append(polys, p)
		}
	}
	if len(polys) == 0 {
		return ""
	}
	in.sig = bytes.Clone(in.sig)
	idx, counts := hint(in.sig)
	p := polys[d.rng.IntN(len(polys))]
	i := polyStart(counts, p) + d.rng.IntN(int(counts[p])-polyStart(counts, p)-1)
	idx[i], idx[i+1] = idx[i+1], idx[i]
	return fmt.Sprintf("hint indices %d and %d swapped", i, i+1)
}

// duplicateHint repeats an index next to itself.
func duplicateHint(d *differ, in *input) string {
	_, counts := hint(in.sig)
	n := int(counts[k-1])
	if n == 0 || n == omega {
		return ""
	}
	in.sig = bytes.Clone(in.sig)
	idx, counts := hint(in.sig)
	i := d.rng.IntN(n)
	copy(idx[i+1:n+1], idx[i:n])
	for p := range k {
		if int(counts[p]) > i {
			counts[p]++
		}
	}
	return fmt.Sprintf("hint index %d repeated", i)
}

// padHintsWithNonzero sets one of the zero bytes after the last index.
func padHintsWithNonzero(d *differ, in *input) string {
	_, counts := hint(in.sig)
	n := int(counts[k-1])
	if n == omega {
		return ""
	}
	in.sig = bytes.Clone(in.sig)
	idx, _ := hint(in.sig)
	i := n + d.rng.IntN(omega-n)
	idx[i] = byte(1 + d.rng.IntN(255))
	return fmt.Sprintf("hint padding byte %d set to %d", i, idx[i])
}

// overflowHintCount sets a count above omega.
func overflowHintCount(d *differ, in *input) string {
	in.sig = bytes.Clone(in.sig)
	_, counts := hint(in.sig)
	p := d.rng.IntN(k)
	counts[p] = byte(omega + 1 + d.rng.IntN(255-omega))
	return fmt.Sprintf("hint count %d set to %d", p, counts[p])
}

// decreaseHintCount sets a count below the one before it.
func decreaseHintCount(d *differ, in *input) string {
	_, counts := hint(in.sig)
	var polys []int
	for p := 1; p < k; p++ {
		if counts[p-1] > 0 {
			polys = append(polys, p)
		}
	}
	if len(polys) == 0 {
		return ""
	}
	in.sig = bytes.Clone(in.sig)
	_, counts = hint(in.sig)
	p := polys[d.rng.IntN(len(polys))]
	counts[p] = byte(d.rng.IntN(int(counts[p-1])))
	return fmt.Sprintf("hint count %d set to %d, below count %d", p, counts[p], p-1)
}

// dropHint removes an index.
func dropHint(d *differ, in *input) string {
	_, counts := hint(in.sig)
	n := int(counts[k-1])
	if n == 0 {
		return ""
	}
	in.sig = bytes.Clone(in.sig)
	idx, counts := hint(in.sig)
	i := d.rng.IntN(n)
	copy(idx[i:n-1], idx[i+1:n])
	idx[n-1] = 0
	for p := range k {
		if int(counts[p]) > i {
			counts[p]--
		}
	}
	return fmt.Sprintf("hint index %d removed", i)
}

// addHint marks another coefficient of a polynomial.
func addHint(d *differ, in *input) string {
	_, counts := hint(in.sig)
	n := int(counts[k-1])
	if n == omega {
		return ""
	}
	in.sig = bytes.Clone(in.sig)
	idx, counts := hint(in.sig)
	p := d.rng.IntN(k)
	start, end := polyStart(counts, p), int(counts[p])
	var marked [256]bool
	for _, c := range idx[start:end] {
		marked[c] = true
	}
	c := byte(d.rng.IntN(256))
	for marked[c] {
		c = byte(d.rng.IntN(256))
	}
	i := start
	for i < end && idx[i] < c {
		i++
	}
	copy(idx[i+1:n+1], idx[i:n])
	idx[i] = c
	for q := p; q < k; q++ {
		counts[q]++
	}
	return fmt.Sprintf("hint for coefficient %d of polynomial %d added", c, p)
}

func flipPublicKeyBit(d *differ, in *input) string {
	return fmt.Sprintf("public key bit %d flipped", d.flipBit(&in.pk, 0, pkSize))
}

func flipMessageBit(d *differ, in *input) string {
	if len(in.msg) == 0 {
		return ""
	}
	return fmt.Sprintf("message bit %d flipped", d.flipBit(&in.msg, 0, len(in.msg)))
}

func appendToMessage(d *differ, in *input) string {
	in.msg = append(bytes.Clone(in.msg), d.bytes(1)...)
	return "byte appended to the message"
}

func truncateMessage(d *differ, in *input) string {
	if len(in.msg) == 0 {
		return ""
	}
	in.msg = in.msg[:d.rng.IntN(len(in.msg))]
	return fmt.Sprintf("message truncated to %d bytes", len(in.msg))
}

func flipContextBit(d *differ, in *input) string {
	if len(in.ctx) == 0 {
		return ""
	}
	return fmt.Sprintf("context bit %d flipped", d.flipBit(&in.ctx, 0, len(in.ctx)))
}

func appendToContext(d *differ, in *input) string {
	in.ctx = append(bytes.Clone(in.ctx), d.bytes(1)...)
	return "byte appended to the context"
}

func truncateContext(d *differ, in *input) string {
	if len(in.ctx) == 0 {
		return ""
	}
	in.ctx = in.ctx[:d.rng.IntN(len(in.ctx))]
	return fmt.Sprintf("context truncated to %d bytes", len(in.ctx))
}

func overlongContext(d *differ, in *input) string {
	n := maxContext + 1 + d.rng.IntN(45)
	in.ctx = append(bytes.Clone(in.ctx), d.bytes(n-len(in.ctx))...)
	return fmt.Sprintf("context extended to %d bytes", n)
}
