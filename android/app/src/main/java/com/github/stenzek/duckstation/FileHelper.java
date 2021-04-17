package com.github.stenzek.duckstation;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;

import java.util.ArrayList;

/**
 * File helper class - used to bridge native code to Java storage access framework APIs.
 */
public class FileHelper {
    /**
     * Native filesystem flags.
     */
    public static final int FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY = 1;
    public static final int FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY = 2;
    public static final int FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED = 4;

    /**
     * Native filesystem find result flags.
     */
    public static final int FILESYSTEM_FIND_RECURSIVE = (1 << 0);
    public static final int FILESYSTEM_FIND_RELATIVE_PATHS = (1 << 1);
    public static final int FILESYSTEM_FIND_HIDDEN_FILES = (1 << 2);
    public static final int FILESYSTEM_FIND_FOLDERS = (1 << 3);
    public static final int FILESYSTEM_FIND_FILES = (1 << 4);
    public static final int FILESYSTEM_FIND_KEEP_ARRAY = (1 << 5);

    /**
     * Projection used when searching for files.
     */
    private static final String[] findProjection = new String[]{
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED
    };

    private final Context context;
    private final ContentResolver contentResolver;

    /**
     * File helper class - used to bridge native code to Java storage access framework APIs.
     * @param context Context in which to perform file actions as.
     */
    public FileHelper(Context context) {
        this.context = context;
        this.contentResolver = context.getContentResolver();
    }

    /**
     * Retrieves a file descriptor for a content URI string. Called by native code.
     * @param uriString string of the URI to open
     * @param mode Java open mode
     * @return file descriptor for URI, or -1
     */
    public int openURIAsFileDescriptor(String uriString, String mode) {
        try {
            final Uri uri = Uri.parse(uriString);
            final ParcelFileDescriptor fd = contentResolver.openFileDescriptor(uri, mode);
            if (fd == null)
                return -1;
            return fd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    /**
     * Recursively iterates documents in the specified tree, searching for files.
     * @param treeUri Root tree in which to search for documents.
     * @param documentId Document ID representing the directory to start searching.
     * @param flags Native search flags.
     * @param results Cumulative result array.
     */
    private void doFindFiles(Uri treeUri, String documentId, int flags, ArrayList<FindResult> results) {
        try {
            final Uri queryUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, documentId);
            final Cursor cursor = contentResolver.query(queryUri, findProjection, null, null, null);
            final int count = cursor.getCount();

            while (cursor.moveToNext()) {
                try {
                    final String mimeType = cursor.getString(2);
                    final String childDocumentId = cursor.getString(0);
                    final Uri uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childDocumentId);
                    final long size = cursor.getLong(3);
                    final long lastModified = cursor.getLong(4);

                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mimeType)) {
                        if ((flags & FILESYSTEM_FIND_FOLDERS) != 0) {
                            results.add(new FindResult(childDocumentId, uri.toString(), size, lastModified, FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY));
                        }

                        if ((flags & FILESYSTEM_FIND_RECURSIVE) != 0)
                            doFindFiles(treeUri, childDocumentId, flags, results);
                    } else {
                        if ((flags & FILESYSTEM_FIND_FILES) != 0) {
                            results.add(new FindResult(childDocumentId, uri.toString(), size, lastModified, 0));
                        }
                    }
                } catch (Exception e) {
                    e.printStackTrace();
                }
            }
            cursor.close();
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    /**
     * Recursively iterates documents in the specified URI, searching for files.
     * @param uriString URI containing directory to search.
     * @param flags Native filter flags.
     * @return Array of find results.
     */
    public FindResult[] findFiles(String uriString, int flags) {
        try {
            final Uri fullUri = Uri.parse(uriString);
            final String documentId = DocumentsContract.getTreeDocumentId(fullUri);
            final ArrayList<FindResult> results = new ArrayList<>();
            doFindFiles(fullUri, documentId, flags, results);
            if (results.isEmpty())
                return null;

            final FindResult[] resultsArray = new FindResult[results.size()];
            results.toArray(resultsArray);
            return resultsArray;
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    /**
     * Java class containing the data for a file in a find operation.
     */
    public static class FindResult {
        public String name;
        public String relativeName;
        public long size;
        public long modifiedTime;
        public int flags;

        public FindResult(String relativeName, String name, long size, long modifiedTime, int flags) {
            this.relativeName = relativeName;
            this.name = name;
            this.size = size;
            this.modifiedTime = modifiedTime;
            this.flags = flags;
        }
    }
}
