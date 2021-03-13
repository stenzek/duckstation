package com.github.stenzek.duckstation;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Helper class for exposing HTTP downloads to native code without pulling in an external
 * dependency for doing so.
 */
public class URLDownloader {
    private int statusCode = -1;
    private byte[] data = null;

    public URLDownloader() {
    }

    static private HttpURLConnection getConnection(String url) {
        try {
            final URL parsedUrl = new URL(url);
            HttpURLConnection connection = (HttpURLConnection) parsedUrl.openConnection();
            if (connection == null)
                throw new RuntimeException(String.format("openConnection(%s) returned null", url));

            return connection;
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    public int getStatusCode() {
        return statusCode;
    }

    public byte[] getData() {
        return data;
    }

    private boolean download(HttpURLConnection connection) {
        try {
            statusCode = connection.getResponseCode();
            if (statusCode != HttpURLConnection.HTTP_OK)
                return false;

            final InputStream inStream = new BufferedInputStream(connection.getInputStream());
            final ByteArrayOutputStream outputStream = new ByteArrayOutputStream();
            final int CHUNK_SIZE = 128 * 1024;
            final byte[] chunk = new byte[CHUNK_SIZE];
            int len;
            while ((len = inStream.read(chunk)) > 0) {
                outputStream.write(chunk, 0, len);
            }

            data = outputStream.toByteArray();
            return true;
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }
    }

    public boolean get(String url) {
        final HttpURLConnection connection = getConnection(url);
        if (connection == null)
            return false;

        return download(connection);
    }

    public boolean post(String url, byte[] postData) {
        final HttpURLConnection connection = getConnection(url);
        if (connection == null)
            return false;

        try {
            connection.setDoOutput(true);
            connection.setChunkedStreamingMode(0);

            OutputStream postStream = new BufferedOutputStream(connection.getOutputStream());
            postStream.write(postData);
            return download(connection);
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }
    }
}
