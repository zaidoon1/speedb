// Copyright (C) 2023 Speedb Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright 2014 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// This test uses a custom FileSystem to keep track of the state of a file
// system the last "Sync". The data being written is cached in a "buffer".
// Only when "Sync" is called, the data will be persistent. It can simulate
// file data loss (or entire files) not protected by a "Sync". For any of the
// FileSystem related operations, by specify the "IOStatus Error", a specific
// error can be returned when file system is not activated.

#include "utilities/fault_injection_fs.h"

#include <algorithm>
#include <functional>
#include <utility>

#include "env/composite_env_wrapper.h"
#include "port/lang.h"
#include "port/stack_trace.h"
#include "test_util/sync_point.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/string_util.h"
#include "util/xxhash.h"

namespace ROCKSDB_NAMESPACE {

const std::string kNewFileNoOverwrite = "";

// Assume a filename, and not a directory name like "/foo/bar/"
std::string TestFSGetDirName(const std::string filename) {
  size_t found = filename.find_last_of("/\\");
  if (found == std::string::npos) {
    return "";
  } else {
    return filename.substr(0, found);
  }
}

// Trim the tailing "/" in the end of `str`
std::string TestFSTrimDirname(const std::string& str) {
  size_t found = str.find_last_not_of("/");
  if (found == std::string::npos) {
    return str;
  }
  return str.substr(0, found + 1);
}

// Return pair <parent directory name, file name> of a full path.
std::pair<std::string, std::string> TestFSGetDirAndName(
    const std::string& name) {
  std::string dirname = TestFSGetDirName(name);
  std::string fname = name.substr(dirname.size() + 1);
  return std::make_pair(dirname, fname);
}

// Calculate the checksum of the data with corresponding checksum
// type. If name does not match, no checksum is returned.
void CalculateTypedChecksum(const ChecksumType& checksum_type, const char* data,
                            size_t size, std::string* checksum) {
  if (checksum_type == ChecksumType::kCRC32c) {
    uint32_t v_crc32c = crc32c::Extend(0, data, size);
    PutFixed32(checksum, v_crc32c);
    return;
  } else if (checksum_type == ChecksumType::kxxHash) {
    uint32_t v = XXH32(data, size, 0);
    PutFixed32(checksum, v);
  }
  return;
}

IOStatus FSFileState::DropUnsyncedData() {
  buffer_.resize(0);
  return IOStatus::OK();
}

IOStatus FSFileState::DropRandomUnsyncedData(Random* rand) {
  const int range = static_cast<int>(buffer_.size());
  if (range > 0) {
    size_t truncated_size = static_cast<size_t>(rand->Uniform(range));
    buffer_.resize(truncated_size);
  }
  return IOStatus::OK();
}

IOStatus TestFSDirectory::Fsync(const IOOptions& options, IODebugContext* dbg) {
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  {
    IOStatus in_s = fs_->InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }
  fs_->SyncDir(dirname_);
  IOStatus s = dir_->Fsync(options, dbg);
  if (s.ok()) {
    IOStatus in_s = fs_->InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }
  return s;
}

IOStatus TestFSDirectory::Close(const IOOptions& options, IODebugContext* dbg) {
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  IOStatus s = dir_->Close(options, dbg);
  return s;
}

IOStatus TestFSDirectory::FsyncWithDirOptions(
    const IOOptions& options, IODebugContext* dbg,
    const DirFsyncOptions& dir_fsync_options) {
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  {
    IOStatus in_s = fs_->InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }
  fs_->SyncDir(dirname_);
  IOStatus s = dir_->FsyncWithDirOptions(options, dbg, dir_fsync_options);
  if (s.ok()) {
    IOStatus in_s = fs_->InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }
  return s;
}

TestFSWritableFile::TestFSWritableFile(const std::string& fname,
                                       const FileOptions& file_opts,
                                       std::unique_ptr<FSWritableFile>&& f,
                                       FaultInjectionTestFS* fs)
    : state_(fname),
      file_opts_(file_opts),
      target_(std::move(f)),
      writable_file_opened_(true),
      fs_(fs) {
  assert(target_ != nullptr);
  state_.pos_ = 0;
}

TestFSWritableFile::~TestFSWritableFile() {
  if (writable_file_opened_) {
    Close(IOOptions(), nullptr).PermitUncheckedError();
  }
}

IOStatus TestFSWritableFile::Append(const Slice& data, const IOOptions& options,
                                    IODebugContext* dbg) {
  MutexLock l(&mutex_);
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  if (target_->use_direct_io()) {
    target_->Append(data, options, dbg).PermitUncheckedError();
  } else {
    state_.buffer_.append(data.data(), data.size());
    state_.pos_ += data.size();
    fs_->WritableFileAppended(state_);
  }
  IOStatus io_s = fs_->InjectWriteError(state_.filename_);
  return io_s;
}

// By setting the IngestDataCorruptionBeforeWrite(), the data corruption is
// simulated.
IOStatus TestFSWritableFile::Append(
    const Slice& data, const IOOptions& options,
    const DataVerificationInfo& verification_info, IODebugContext* dbg) {
  MutexLock l(&mutex_);
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  if (fs_->ShouldDataCorruptionBeforeWrite()) {
    return IOStatus::Corruption("Data is corrupted!");
  }

  // Calculate the checksum
  std::string checksum;
  CalculateTypedChecksum(fs_->GetChecksumHandoffFuncType(), data.data(),
                         data.size(), &checksum);
  if (fs_->GetChecksumHandoffFuncType() != ChecksumType::kNoChecksum &&
      checksum != verification_info.checksum.ToString()) {
    std::string msg = "Data is corrupted! Origin data checksum: " +
                      verification_info.checksum.ToString() +
                      "current data checksum: " + checksum;
    return IOStatus::Corruption(msg);
  }
  if (target_->use_direct_io()) {
    target_->Append(data, options, dbg).PermitUncheckedError();
  } else {
    state_.buffer_.append(data.data(), data.size());
    state_.pos_ += data.size();
    fs_->WritableFileAppended(state_);
  }
  IOStatus io_s = fs_->InjectWriteError(state_.filename_);
  return io_s;
}

IOStatus TestFSWritableFile::PositionedAppend(
    const Slice& data, uint64_t offset, const IOOptions& options,
    const DataVerificationInfo& verification_info, IODebugContext* dbg) {
  MutexLock l(&mutex_);
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  if (fs_->ShouldDataCorruptionBeforeWrite()) {
    return IOStatus::Corruption("Data is corrupted!");
  }

  // Calculate the checksum
  std::string checksum;
  CalculateTypedChecksum(fs_->GetChecksumHandoffFuncType(), data.data(),
                         data.size(), &checksum);
  if (fs_->GetChecksumHandoffFuncType() != ChecksumType::kNoChecksum &&
      checksum != verification_info.checksum.ToString()) {
    std::string msg = "Data is corrupted! Origin data checksum: " +
                      verification_info.checksum.ToString() +
                      "current data checksum: " + checksum;
    return IOStatus::Corruption(msg);
  }
  target_->PositionedAppend(data, offset, options, dbg);
  IOStatus io_s = fs_->InjectWriteError(state_.filename_);
  return io_s;
}

IOStatus TestFSWritableFile::Close(const IOOptions& options,
                                   IODebugContext* dbg) {
  MutexLock l(&mutex_);
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  {
    IOStatus in_s = fs_->InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }
  writable_file_opened_ = false;
  IOStatus io_s;
  if (!target_->use_direct_io()) {
    io_s = target_->Append(state_.buffer_, options, dbg);
  }
  if (io_s.ok()) {
    state_.buffer_.resize(0);
    // Ignore sync errors
    target_->Sync(options, dbg).PermitUncheckedError();
    io_s = target_->Close(options, dbg);
  }
  if (io_s.ok()) {
    fs_->WritableFileClosed(state_);
    IOStatus in_s = fs_->InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }
  return io_s;
}

IOStatus TestFSWritableFile::Flush(const IOOptions&, IODebugContext*) {
  MutexLock l(&mutex_);
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  if (fs_->IsFilesystemActive()) {
    state_.pos_at_last_flush_ = state_.pos_;
  }
  return IOStatus::OK();
}

IOStatus TestFSWritableFile::Sync(const IOOptions& options,
                                  IODebugContext* dbg) {
  MutexLock l(&mutex_);
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  if (target_->use_direct_io()) {
    // For Direct IO mode, we don't buffer anything in TestFSWritableFile.
    // So just return
    return IOStatus::OK();
  }
  IOStatus io_s = target_->Append(state_.buffer_, options, dbg);
  state_.buffer_.resize(0);
  // Ignore sync errors
  target_->Sync(options, dbg).PermitUncheckedError();
  state_.pos_at_last_sync_ = state_.pos_;
  fs_->WritableFileSynced(state_);
  return io_s;
}

IOStatus FaultInjectionTestFS::DoWrite(FSRandomRWFile* file, uint64_t offset,
                                       const Slice& data,
                                       const IOOptions& options,
                                       IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  } else {
    return file->Write(offset, data, options, dbg);
  }
}

IOStatus TestFSWritableFile::RangeSync(uint64_t offset, uint64_t nbytes,
                                       const IOOptions& options,
                                       IODebugContext* dbg) {
  MutexLock l(&mutex_);
  if (!fs_->IsFilesystemActive()) {
    return fs_->GetError();
  }
  // Assumes caller passes consecutive byte ranges.
  uint64_t sync_limit = offset + nbytes;
  uint64_t buf_begin =
      state_.pos_at_last_sync_ < 0 ? 0 : state_.pos_at_last_sync_;

  IOStatus io_s;
  if (sync_limit < buf_begin) {
    return io_s;
  }
  uint64_t num_to_sync = std::min(static_cast<uint64_t>(state_.buffer_.size()),
                                  sync_limit - buf_begin);
  Slice buf_to_sync(state_.buffer_.data(), num_to_sync);
  io_s = target_->Append(buf_to_sync, options, dbg);
  state_.buffer_ = state_.buffer_.substr(num_to_sync);
  // Ignore sync errors
  target_->RangeSync(offset, nbytes, options, dbg).PermitUncheckedError();
  state_.pos_at_last_sync_ = offset + num_to_sync;
  fs_->WritableFileSynced(state_);
  return io_s;
}

IOStatus FaultInjectionTestFS::DoRead(FSRandomRWFile* file, uint64_t offset,
                                      size_t n, const IOOptions& options,
                                      Slice* result, char* scratch,
                                      IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  } else {
    return file->Read(offset, n, options, result, scratch, dbg);
  }
}

IOStatus FaultInjectionTestFS::DoClose(FSRandomRWFile* file,
                                       const IOOptions& options,
                                       IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  } else {
    return file->Close(options, dbg);
  }
}
IOStatus FaultInjectionTestFS::DoFlush(FSRandomRWFile* file,
                                       const IOOptions& options,
                                       IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  } else {
    return file->Flush(options, dbg);
  }
}

IOStatus FaultInjectionTestFS::DoSync(FSRandomRWFile* file,
                                      const IOOptions& options,
                                      IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  } else {
    return file->Sync(options, dbg);
  }
}

IOStatus FaultInjectionTestFS::DoRead(FSRandomAccessFile* file, uint64_t offset,
                                      size_t n, const IOOptions& options,
                                      Slice* result, char* scratch,
                                      IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  IOStatus s = file->Read(offset, n, options, result, scratch, dbg);
  if (s.ok()) {
    s = InjectThreadSpecificReadError(
        FaultInjectionTestFS::ErrorOperation::kRead, result,
        file->use_direct_io(), scratch, /*need_count_increase=*/true,
        /*fault_injected=*/nullptr);
  }
  if (s.ok() && ShouldInjectRandomReadError()) {
    return IOStatus::IOError("Injected read error");
  }
  return s;
}

IOStatus FaultInjectionTestFS::DoReadAsync(
    FSRandomAccessFile* file, FSReadRequest& req, const IOOptions& opts,
    std::function<void(const FSReadRequest&, void*)> cb, void* cb_arg,
    void** io_handle, IOHandleDeleter* del_fn, IODebugContext* dbg) {
  IOStatus ret;
  IOStatus s;
  FSReadRequest res;
  if (!IsFilesystemActive()) {
    ret = GetError();
  } else {
    ret = InjectThreadSpecificReadError(
        FaultInjectionTestFS::ErrorOperation::kRead, &res.result,
        file->use_direct_io(), req.scratch, /*need_count_increase=*/true,
        /*fault_injected=*/nullptr);
  }
  if (ret.ok()) {
    if (ShouldInjectRandomReadError()) {
      ret = IOStatus::IOError("Injected read error");
    } else {
      s = file->ReadAsync(req, opts, cb, cb_arg, io_handle, del_fn, dbg);
    }
  }
  if (!ret.ok()) {
    res.status = ret;
    cb(res, cb_arg);
  }
  return s;
}

IOStatus FaultInjectionTestFS::DoMultiRead(FSRandomAccessFile* file,
                                           FSReadRequest* reqs, size_t num_reqs,
                                           const IOOptions& options,
                                           IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  IOStatus s = file->MultiRead(reqs, num_reqs, options, dbg);
  bool injected_error = false;
  for (size_t i = 0; i < num_reqs; i++) {
    if (!reqs[i].status.ok()) {
      // Already seeing an error.
      break;
    }
    bool this_injected_error;
    reqs[i].status = InjectThreadSpecificReadError(
        FaultInjectionTestFS::ErrorOperation::kMultiReadSingleReq,
        &(reqs[i].result), file->use_direct_io(), reqs[i].scratch,
        /*need_count_increase=*/true,
        /*fault_injected=*/&this_injected_error);
    injected_error |= this_injected_error;
  }
  if (s.ok()) {
    s = InjectThreadSpecificReadError(
        FaultInjectionTestFS::ErrorOperation::kMultiRead, nullptr,
        file->use_direct_io(), nullptr, /*need_count_increase=*/!injected_error,
        /*fault_injected=*/nullptr);
  }
  if (s.ok() && ShouldInjectRandomReadError()) {
    return IOStatus::IOError("Injected read error");
  }
  return s;
}

size_t FaultInjectionTestFS::DoGetUniqueId(FSRandomAccessFile* file, char* id,
                                           size_t max_size) {
  if (ShouldFailGetUniqueId()) {
    return 0;
  } else {
    return file->GetUniqueId(id, max_size);
  }
}

IOStatus FaultInjectionTestFS::DoRead(FSSequentialFile* file, size_t n,
                                      const IOOptions& options, Slice* result,
                                      char* scratch, IODebugContext* dbg) {
  IOStatus s = file->Read(n, options, result, scratch, dbg);
  if (s.ok() && ShouldInjectRandomReadError()) {
    return IOStatus::IOError("Injected seq read error");
  }
  return s;
}

IOStatus FaultInjectionTestFS::DoPositionedRead(FSSequentialFile* file,
                                                uint64_t offset, size_t n,
                                                const IOOptions& options,
                                                Slice* result, char* scratch,
                                                IODebugContext* dbg) {
  IOStatus s = file->PositionedRead(offset, n, options, result, scratch, dbg);
  if (s.ok() && ShouldInjectRandomReadError()) {
    return IOStatus::IOError("Injected seq positioned read error");
  }
  return s;
}

IOStatus FaultInjectionTestFS::NewDirectory(
    const std::string& name, const IOOptions& options,
    std::unique_ptr<FSDirectory>* result, IODebugContext* dbg) {
  std::unique_ptr<FSDirectory> r;
  IOStatus io_s = target()->NewDirectory(name, options, &r, dbg);
  if (!io_s.ok()) {
    return io_s;
  }
  result->reset(
      new TestFSDirectory(this, TestFSTrimDirname(name), r.release()));
  return IOStatus::OK();
}

IOStatus FaultInjectionTestFS::NewWritableFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSWritableFile>* result, IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  {
    IOStatus in_s = InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }

  if (ShouldUseDirectWritable(fname)) {
    return target()->NewWritableFile(fname, file_opts, result, dbg);
  }

  IOStatus io_s = target()->NewWritableFile(fname, file_opts, result, dbg);
  if (io_s.ok()) {
    result->reset(
        new TestFSWritableFile(fname, file_opts, std::move(*result), this));
    // WritableFileWriter* file is opened
    // again then it will be truncated - so forget our saved state.
    UntrackFile(fname);
    {
      MutexLock l(&mutex_);
      open_managed_files_.insert(fname);
      auto dir_and_name = TestFSGetDirAndName(fname);
      auto& list = dir_to_new_files_since_last_sync_[dir_and_name.first];
      // The new file could overwrite an old one. Here we simplify
      // the implementation by assuming no file of this name after
      // dropping unsynced files.
      list[dir_and_name.second] = kNewFileNoOverwrite;
    }
    {
      IOStatus in_s = InjectMetadataWriteError();
      if (!in_s.ok()) {
        return in_s;
      }
    }
  }
  return io_s;
}

IOStatus FaultInjectionTestFS::ReopenWritableFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSWritableFile>* result, IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  if (ShouldUseDirectWritable(fname)) {
    return target()->ReopenWritableFile(fname, file_opts, result, dbg);
  }
  {
    IOStatus in_s = InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }

  bool exists;
  IOStatus io_s,
      exists_s = target()->FileExists(fname, IOOptions(), nullptr /* dbg */);
  if (exists_s.IsNotFound()) {
    exists = false;
  } else if (exists_s.ok()) {
    exists = true;
  } else {
    io_s = exists_s;
    exists = false;
  }

  if (io_s.ok()) {
    io_s = target()->ReopenWritableFile(fname, file_opts, result, dbg);
  }

  // Only track files we created. Files created outside of this
  // `FaultInjectionTestFS` are not eligible for tracking/data dropping
  // (for example, they may contain data a previous db_stress run expects to
  // be recovered). This could be extended to track/drop data appended once
  // the file is under `FaultInjectionTestFS`'s control.
  if (io_s.ok()) {
    bool should_track;
    {
      MutexLock l(&mutex_);
      if (db_file_state_.find(fname) != db_file_state_.end()) {
        // It was written by this `FileSystem` earlier.
        assert(exists);
        should_track = true;
      } else if (!exists) {
        // It was created by this `FileSystem` just now.
        should_track = true;
        open_managed_files_.insert(fname);
        auto dir_and_name = TestFSGetDirAndName(fname);
        auto& list = dir_to_new_files_since_last_sync_[dir_and_name.first];
        list[dir_and_name.second] = kNewFileNoOverwrite;
      } else {
        should_track = false;
      }
    }
    if (should_track) {
      result->reset(
          new TestFSWritableFile(fname, file_opts, std::move(*result), this));
    }
    {
      IOStatus in_s = InjectMetadataWriteError();
      if (!in_s.ok()) {
        return in_s;
      }
    }
  }
  return io_s;
}

IOStatus FaultInjectionTestFS::NewRandomRWFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSRandomRWFile>* result, IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  if (ShouldUseDirectWritable(fname)) {
    return target()->NewRandomRWFile(fname, file_opts, result, dbg);
  } else {
    IOStatus in_s = InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }
  IOStatus io_s =
      InjectionFileSystem::NewRandomRWFile(fname, file_opts, result, dbg);
  if (io_s.ok()) {
    // WritableFileWriter* file is opened
    // again then it will be truncated - so forget our saved state.
    UntrackFile(fname);
    {
      MutexLock l(&mutex_);
      open_managed_files_.insert(fname);
      auto dir_and_name = TestFSGetDirAndName(fname);
      auto& list = dir_to_new_files_since_last_sync_[dir_and_name.first];
      // It could be overwriting an old file, but we simplify the
      // implementation by ignoring it.
      list[dir_and_name.second] = kNewFileNoOverwrite;
    }
    {
      IOStatus in_s = InjectMetadataWriteError();
      if (!in_s.ok()) {
        return in_s;
      }
    }
  }
  return io_s;
}

IOStatus FaultInjectionTestFS::NewRandomAccessFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSRandomAccessFile>* result, IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  if (ShouldInjectRandomReadError()) {
    return IOStatus::IOError("Injected error when open random access file");
  }
  IOStatus io_s = InjectThreadSpecificReadError(ErrorOperation::kOpen, nullptr,
                                                false, nullptr,
                                                /*need_count_increase=*/true,
                                                /*fault_injected=*/nullptr);
  if (io_s.ok()) {
    io_s =
        InjectionFileSystem::NewRandomAccessFile(fname, file_opts, result, dbg);
  }
  return io_s;
}

IOStatus FaultInjectionTestFS::NewSequentialFile(
    const std::string& fname, const FileOptions& file_opts,
    std::unique_ptr<FSSequentialFile>* result, IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  } else if (ShouldInjectRandomReadError()) {
    return IOStatus::IOError("Injected read error when creating seq file");
  } else {
    return InjectionFileSystem::NewSequentialFile(fname, file_opts, result,
                                                  dbg);
  }
}

IOStatus FaultInjectionTestFS::DeleteFile(const std::string& f,
                                          const IOOptions& options,
                                          IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  {
    IOStatus in_s = InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }
  IOStatus io_s = InjectionFileSystem::DeleteFile(f, options, dbg);
  if (io_s.ok()) {
    UntrackFile(f);
    {
      IOStatus in_s = InjectMetadataWriteError();
      if (!in_s.ok()) {
        return in_s;
      }
    }
  }
  return io_s;
}

IOStatus FaultInjectionTestFS::RenameFile(const std::string& s,
                                          const std::string& t,
                                          const IOOptions& options,
                                          IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  {
    IOStatus in_s = InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }

  // We preserve contents of overwritten files up to a size threshold.
  // We could keep previous file in another name, but we need to worry about
  // garbage collect the those files. We do it if it is needed later.
  // We ignore I/O errors here for simplicity.
  std::string previous_contents = kNewFileNoOverwrite;
  if (target()->FileExists(t, IOOptions(), nullptr).ok()) {
    uint64_t file_size;
    if (target()->GetFileSize(t, IOOptions(), &file_size, nullptr).ok() &&
        file_size < 1024) {
      ReadFileToString(target(), t, &previous_contents).PermitUncheckedError();
    }
  }
  IOStatus io_s = InjectionFileSystem::RenameFile(s, t, options, dbg);

  if (io_s.ok()) {
    {
      MutexLock l(&mutex_);
      if (db_file_state_.find(s) != db_file_state_.end()) {
        db_file_state_[t] = db_file_state_[s];
        db_file_state_.erase(s);
      }

      auto sdn = TestFSGetDirAndName(s);
      auto tdn = TestFSGetDirAndName(t);
      if (dir_to_new_files_since_last_sync_[sdn.first].erase(sdn.second) != 0) {
        auto& tlist = dir_to_new_files_since_last_sync_[tdn.first];
        assert(tlist.find(tdn.second) == tlist.end());
        tlist[tdn.second] = previous_contents;
      }
    }
    IOStatus in_s = InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }

  return io_s;
}

IOStatus FaultInjectionTestFS::LinkFile(const std::string& s,
                                        const std::string& t,
                                        const IOOptions& options,
                                        IODebugContext* dbg) {
  if (!IsFilesystemActive()) {
    return GetError();
  }
  {
    IOStatus in_s = InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }

  // Using the value in `dir_to_new_files_since_last_sync_` for the source file
  // may be a more reasonable choice.
  std::string previous_contents = kNewFileNoOverwrite;

  IOStatus io_s = InjectionFileSystem::LinkFile(s, t, options, dbg);

  if (io_s.ok()) {
    {
      MutexLock l(&mutex_);
      if (db_file_state_.find(s) != db_file_state_.end()) {
        db_file_state_[t] = db_file_state_[s];
      }

      auto sdn = TestFSGetDirAndName(s);
      auto tdn = TestFSGetDirAndName(t);
      if (dir_to_new_files_since_last_sync_[sdn.first].find(sdn.second) !=
          dir_to_new_files_since_last_sync_[sdn.first].end()) {
        auto& tlist = dir_to_new_files_since_last_sync_[tdn.first];
        assert(tlist.find(tdn.second) == tlist.end());
        tlist[tdn.second] = previous_contents;
      }
    }
    IOStatus in_s = InjectMetadataWriteError();
    if (!in_s.ok()) {
      return in_s;
    }
  }

  return io_s;
}

IOStatus FaultInjectionTestFS::Poll(std::vector<void*>& io_handles,
                                    size_t min_completions) {
  return target()->Poll(io_handles, min_completions);
}

IOStatus FaultInjectionTestFS::AbortIO(std::vector<void*>& io_handles) {
  return target()->AbortIO(io_handles);
}

void FaultInjectionTestFS::WritableFileClosed(const FSFileState& state) {
  MutexLock l(&mutex_);
  if (open_managed_files_.find(state.filename_) != open_managed_files_.end()) {
    db_file_state_[state.filename_] = state;
    open_managed_files_.erase(state.filename_);
  }
}

void FaultInjectionTestFS::WritableFileSynced(const FSFileState& state) {
  MutexLock l(&mutex_);
  if (open_managed_files_.find(state.filename_) != open_managed_files_.end()) {
    if (db_file_state_.find(state.filename_) == db_file_state_.end()) {
      db_file_state_.insert(std::make_pair(state.filename_, state));
    } else {
      db_file_state_[state.filename_] = state;
    }
  }
}

void FaultInjectionTestFS::WritableFileAppended(const FSFileState& state) {
  MutexLock l(&mutex_);
  if (open_managed_files_.find(state.filename_) != open_managed_files_.end()) {
    if (db_file_state_.find(state.filename_) == db_file_state_.end()) {
      db_file_state_.insert(std::make_pair(state.filename_, state));
    } else {
      db_file_state_[state.filename_] = state;
    }
  }
}

IOStatus FaultInjectionTestFS::DropUnsyncedFileData() {
  IOStatus io_s;
  MutexLock l(&mutex_);
  for (std::map<std::string, FSFileState>::iterator it = db_file_state_.begin();
       io_s.ok() && it != db_file_state_.end(); ++it) {
    FSFileState& fs_state = it->second;
    if (!fs_state.IsFullySynced()) {
      io_s = fs_state.DropUnsyncedData();
    }
  }
  return io_s;
}

IOStatus FaultInjectionTestFS::DropRandomUnsyncedFileData(Random* rnd) {
  IOStatus io_s;
  MutexLock l(&mutex_);
  for (std::map<std::string, FSFileState>::iterator it = db_file_state_.begin();
       io_s.ok() && it != db_file_state_.end(); ++it) {
    FSFileState& fs_state = it->second;
    if (!fs_state.IsFullySynced()) {
      io_s = fs_state.DropRandomUnsyncedData(rnd);
    }
  }
  return io_s;
}

IOStatus FaultInjectionTestFS::DeleteFilesCreatedAfterLastDirSync(
    const IOOptions& options, IODebugContext* dbg) {
  // Because DeleteFile access this container make a copy to avoid deadlock
  std::map<std::string, std::map<std::string, std::string>> map_copy;
  {
    MutexLock l(&mutex_);
    map_copy.insert(dir_to_new_files_since_last_sync_.begin(),
                    dir_to_new_files_since_last_sync_.end());
  }

  for (auto& pair : map_copy) {
    for (auto& file_pair : pair.second) {
      if (file_pair.second == kNewFileNoOverwrite) {
        IOStatus io_s =
            DeleteFile(pair.first + "/" + file_pair.first, options, dbg);
        if (!io_s.ok()) {
          return io_s;
        }
      } else {
        IOStatus io_s =
            WriteStringToFile(target(), file_pair.second,
                              pair.first + "/" + file_pair.first, true);
        if (!io_s.ok()) {
          return io_s;
        }
      }
    }
  }
  return IOStatus::OK();
}

void FaultInjectionTestFS::ResetState() {
  MutexLock l(&mutex_);
  db_file_state_.clear();
  dir_to_new_files_since_last_sync_.clear();
  SetFilesystemActiveNoLock(true);
}

void FaultInjectionTestFS::UntrackFile(const std::string& f) {
  MutexLock l(&mutex_);
  auto dir_and_name = TestFSGetDirAndName(f);
  dir_to_new_files_since_last_sync_[dir_and_name.first].erase(
      dir_and_name.second);
  db_file_state_.erase(f);
  open_managed_files_.erase(f);
}

IOStatus FaultInjectionTestFS::InjectThreadSpecificReadError(
    ErrorOperation op, Slice* result, bool direct_io, char* scratch,
    bool need_count_increase, bool* fault_injected) {
  bool dummy_bool;
  bool& ret_fault_injected = fault_injected ? *fault_injected : dummy_bool;
  ret_fault_injected = false;
  ErrorContext* ctx = static_cast<ErrorContext*>(thread_local_error_->Get());
  if (ctx == nullptr || !ctx->enable_error_injection || !ctx->one_in) {
    return IOStatus::OK();
  }

  if (ctx->rand.OneIn(ctx->one_in)) {
    if (ctx->count == 0) {
      ctx->message = "";
    }
    if (need_count_increase) {
      ctx->count++;
    }
    if (ctx->callstack) {
      free(ctx->callstack);
    }
    ctx->callstack = port::SaveStack(&ctx->frames);

    if (op != ErrorOperation::kMultiReadSingleReq) {
      // Likely non-per read status code for MultiRead
      ctx->message += "error; ";
      ret_fault_injected = true;
      return IOStatus::IOError();
    } else if (Random::GetTLSInstance()->OneIn(8)) {
      assert(result);
      // For a small chance, set the failure to status but turn the
      // result to be empty, which is supposed to be caught for a check.
      *result = Slice();
      ctx->message += "inject empty result; ";
      ret_fault_injected = true;
    } else if (!direct_io && Random::GetTLSInstance()->OneIn(7) &&
               scratch != nullptr && result->data() == scratch) {
      assert(result);
      // With direct I/O, many extra bytes might be read so corrupting
      // one byte might not cause checksum mismatch. Skip checksum
      // corruption injection.
      // We only corrupt data if the result is filled to `scratch`. For other
      // cases, the data might not be able to be modified (e.g mmaped files)
      // or has unintended side effects.
      // For a small chance, set the failure to status but corrupt the
      // result in a way that checksum checking is supposed to fail.
      // Corrupt the last byte, which is supposed to be a checksum byte
      // It would work for CRC. Not 100% sure for xxhash and will adjust
      // if it is not the case.
      const_cast<char*>(result->data())[result->size() - 1]++;
      ctx->message += "corrupt last byte; ";
      ret_fault_injected = true;
    } else {
      ctx->message += "error result multiget single; ";
      ret_fault_injected = true;
      return IOStatus::IOError();
    }
  }
  return IOStatus::OK();
}

bool FaultInjectionTestFS::TryParseFileName(const std::string& file_name,
                                            uint64_t* number, FileType* type) {
  std::size_t found = file_name.find_last_of("/");
  std::string file = file_name.substr(found);
  return ParseFileName(file, number, type);
}

IOStatus FaultInjectionTestFS::InjectWriteError(const std::string& file_name) {
  MutexLock l(&mutex_);
  if (!enable_write_error_injection_ || !write_error_one_in_) {
    return IOStatus::OK();
  }
  bool allowed_type = false;

  if (inject_for_all_file_types_) {
    allowed_type = true;
  } else {
    uint64_t number;
    FileType cur_type = kTempFile;
    if (TryParseFileName(file_name, &number, &cur_type)) {
      for (const auto& type : write_error_allowed_types_) {
        if (cur_type == type) {
          allowed_type = true;
        }
      }
    }
  }

  if (allowed_type) {
    if (write_error_rand_.OneIn(write_error_one_in_)) {
      return GetError();
    }
  }
  return IOStatus::OK();
}

IOStatus FaultInjectionTestFS::InjectMetadataWriteError() {
  {
    MutexLock l(&mutex_);
    if (!enable_metadata_write_error_injection_ ||
        !metadata_write_error_one_in_ ||
        !write_error_rand_.OneIn(metadata_write_error_one_in_)) {
      return IOStatus::OK();
    }
  }
  TEST_SYNC_POINT("FaultInjectionTestFS::InjectMetadataWriteError:Injected");
  return IOStatus::IOError();
}

void FaultInjectionTestFS::PrintFaultBacktrace() {
#if defined(OS_LINUX)
  ErrorContext* ctx = static_cast<ErrorContext*>(thread_local_error_->Get());
  if (ctx == nullptr) {
    return;
  }
  fprintf(stderr, "Injected error type = %d\n", ctx->type);
  fprintf(stderr, "Message: %s\n", ctx->message.c_str());
  port::PrintAndFreeStack(ctx->callstack, ctx->frames);
  ctx->callstack = nullptr;
#endif
}

}  // namespace ROCKSDB_NAMESPACE
