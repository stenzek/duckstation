package com.github.stenzek.duckstation;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;

public class MemoryCardImage {
    public static final int DATA_SIZE = 128 * 1024;
    public static final String FILE_EXTENSION = ".mcd";

    private final Context context;
    private final Uri uri;
    private final byte[] data;

    private MemoryCardImage(Context context, Uri uri, byte[] data) {
        this.context = context;
        this.uri = uri;
        this.data = data;
    }

    public static String getTitleForUri(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name != null) {
            final int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0)
                name = name.substring(lastSlash + 1);

            if (name.endsWith(FILE_EXTENSION))
                name = name.substring(0, name.length() - FILE_EXTENSION.length());
        } else {
            name = uri.toString();
        }

        return name;
    }

    public static MemoryCardImage open(Context context, Uri uri) {
        byte[] data = FileUtil.readBytesFromUri(context, uri, DATA_SIZE);
        if (data == null)
            return null;

        if (!isValid(data))
            return null;

        return new MemoryCardImage(context, uri, data);
    }

    public static MemoryCardImage create(Context context, Uri uri) {
        byte[] data = new byte[DATA_SIZE];
        format(data);

        MemoryCardImage card = new MemoryCardImage(context, uri, data);
        if (!card.save())
            return null;

        return card;
    }

    public static Uri[] getCardUris(Context context) {
        final String directory = String.format("%s/memcards",
                AndroidHostInterface.getUserDirectory());
        final ArrayList<Uri> results = new ArrayList<>();

        if (directory.charAt(0) == '/') {
            // native path
            final File directoryFile = new File(directory);
            final File[] files = directoryFile.listFiles();
            for (File file : files) {
                if (!file.isFile())
                    continue;

                if (!file.getName().endsWith(FILE_EXTENSION))
                    continue;

                results.add(Uri.fromFile(file));
            }
        } else {
            try {
                final Uri baseUri = null;
                final String[] scanProjection = new String[]{
                        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                        DocumentsContract.Document.COLUMN_MIME_TYPE};
                final ContentResolver resolver = context.getContentResolver();
                final String treeDocId = DocumentsContract.getTreeDocumentId(baseUri);
                final Uri queryUri = DocumentsContract.buildChildDocumentsUriUsingTree(baseUri, treeDocId);
                final Cursor cursor = resolver.query(queryUri, scanProjection, null, null, null);

                while (cursor.moveToNext()) {
                    try {
                        final String mimeType = cursor.getString(2);
                        final String documentId = cursor.getString(0);
                        final Uri uri = DocumentsContract.buildDocumentUriUsingTree(baseUri, documentId);
                        if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mimeType)) {
                            continue;
                        }

                        final String uriString = uri.toString();
                        if (!uriString.endsWith(FILE_EXTENSION))
                            continue;

                        results.add(uri);
                    } catch (Exception e) {
                        e.printStackTrace();
                    }
                }
                cursor.close();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        if (results.isEmpty())
            return null;

        Collections.sort(results, (a, b) -> a.compareTo(b));

        final Uri[] resultArray = new Uri[results.size()];
        results.toArray(resultArray);
        return resultArray;
    }

    private static native boolean isValid(byte[] data);

    private static native void format(byte[] data);

    private static native int getFreeBlocks(byte[] data);

    private static native MemoryCardFileInfo[] getFiles(byte[] data);

    private static native boolean hasFile(byte[] data, String filename);

    private static native byte[] readFile(byte[] data, String filename);

    private static native boolean writeFile(byte[] data, String filename, byte[] fileData);

    private static native boolean deleteFile(byte[] data, String filename);

    private static native boolean importCard(byte[] data, String filename, byte[] importData);

    public boolean save() {
        return FileUtil.writeBytesToUri(context, uri, data);
    }

    public boolean delete() {
        return FileUtil.deleteFileAtUri(context, uri);
    }

    public boolean format() {
        format(data);
        return save();
    }

    public Uri getUri() {
        return uri;
    }

    public String getTitle() {
        return getTitleForUri(uri);
    }

    public int getFreeBlocks() {
        return getFreeBlocks(data);
    }

    public MemoryCardFileInfo[] getFiles() {
        return getFiles(data);
    }

    public boolean hasFile(String filename) {
        return hasFile(data, filename);
    }

    public byte[] readFile(String filename) {
        return readFile(data, filename);
    }

    public boolean writeFile(String filename, byte[] fileData) {
        if (!writeFile(data, filename, fileData))
            return false;

        return save();
    }

    public boolean deleteFile(String filename) {
        if (!deleteFile(data, filename))
            return false;

        return save();
    }

    public boolean importCard(String filename, byte[] importData) {
        if (!importCard(data, filename, importData))
            return false;

        return save();
    }
}
