#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hogd {

// HogDevnodeTmpDir owns a directory a devnode subsystem writes into: either a
// throwaway temp dir that is removed again on destruction, or a subdirectory of
// an operator-supplied data dir that outlives the process.
//
// Acquiring one fails on operator environment problems -- an unwritable
// $TMPDIR, a data dir that is really a file -- rather than on bugs, so it is
// reachable only through [create], which reports such a failure as a message
// for the caller to log. Nothing here throws.
class HogDevnodeTmpDir {
public:
  // create returns the directory [data_dir]/[subdir], creating it if needed and
  // keeping it on destruction; when [data_dir] is absent it instead returns a
  // fresh temp dir that is removed on destruction. The error is a diagnostic
  // naming the offending path and how to fix it.
  static std::expected<HogDevnodeTmpDir, std::string>
  create(std::optional<std::filesystem::path> const &data_dir,
         std::string_view subdir);

  ~HogDevnodeTmpDir() noexcept;

  HogDevnodeTmpDir(HogDevnodeTmpDir const &) = delete;
  HogDevnodeTmpDir &operator=(HogDevnodeTmpDir const &) = delete;
  // Moving hands the directory -- and the duty to remove it -- to the
  // destination; the source is left owning nothing.
  HogDevnodeTmpDir(HogDevnodeTmpDir &&other) noexcept;
  HogDevnodeTmpDir &operator=(HogDevnodeTmpDir &&other) noexcept;

  std::string const &directory() const { return path_; }

private:
  HogDevnodeTmpDir(std::string path, bool remove_on_destroy)
    : path_(std::move(path)), remove_on_destroy_(remove_on_destroy)
  {}

  // release removes the owned directory, if this instance owns one, and leaves
  // the instance owning nothing.
  void release() noexcept;

  std::string path_;
  bool remove_on_destroy_ = false;
};

// HogDevnodeDirectories holds the working directories a devnode's subsystems
// write into. The node takes them already-created so that an unusable $TMPDIR
// or data dir is reported to the operator before the node starts building
// runtime services -- see [create].
struct HogDevnodeDirectories {
  // create opens all four directories under [data_dir], or as throwaway temp
  // dirs when [data_dir] is absent. The error is a diagnostic to log before
  // exiting; temp dirs opened before the failing one are removed on the way
  // out, while directories under an operator's [data_dir] are left in place.
  static std::expected<HogDevnodeDirectories, std::string>
  create(std::optional<std::filesystem::path> const &data_dir);

  HogDevnodeTmpDir runtime;
  HogDevnodeTmpDir db;
  HogDevnodeTmpDir event;
  HogDevnodeTmpDir indexer;
};

} // namespace hogd
