package com.agoessling.swingcapture;

import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;

/** Android implementation of a directory fsync after an atomic file publication. */
final class AndroidDirectorySync {
  private AndroidDirectorySync() {}

  static void synchronize(File directory) throws IOException {
    FileDescriptor descriptor = null;
    try {
      descriptor = Os.open(directory.getAbsolutePath(), OsConstants.O_RDONLY, 0);
      Os.fsync(descriptor);
    } catch (ErrnoException failure) {
      throw new IOException("Unable to synchronize coordination directory", failure);
    } finally {
      if (descriptor != null) {
        try {
          Os.close(descriptor);
        } catch (ErrnoException failure) {
          throw new IOException("Unable to close coordination directory", failure);
        }
      }
    }
  }
}
