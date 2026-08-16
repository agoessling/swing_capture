package com.agoessling.swingcapture;

import android.content.Context;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** Atomically stores the latest capability or capture-probe report. */
public final class ReportStore {
  private ReportStore() {}

  public static synchronized File writeLatest(Context context, String json) throws IOException {
    File directory = new File(context.getFilesDir(), "reports");
    if (!directory.isDirectory() && !directory.mkdirs()) {
      throw new IOException("Unable to create report directory " + directory);
    }
    File temporary = new File(directory, "latest.json.tmp");
    File destination = new File(directory, "latest.json");
    try (FileOutputStream output = new FileOutputStream(temporary)) {
      output.write(json.getBytes(StandardCharsets.UTF_8));
      output.getFD().sync();
    }
    if (!temporary.renameTo(destination)) {
      throw new IOException("Unable to publish report " + destination);
    }
    return destination;
  }
}
