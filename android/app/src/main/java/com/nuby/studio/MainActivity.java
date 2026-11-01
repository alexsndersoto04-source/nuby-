package com.nuby.studio;

import android.Manifest;
import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.MediaStore;
import android.util.Base64;
import android.util.Log;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.PermissionRequest;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Contenedor nativo de Nuby Studio.
 *
 * La aplicación es el mismo estudio que funciona en el navegador, servido desde
 * https://localhost (origen seguro: sin esto el micrófono y los AudioWorklet no
 * funcionan). Los archivos se leen del propio APK, no de internet.
 *
 * Lo que solo se puede hacer en nativo está aquí:
 *  - Permiso de micrófono (RECORD_AUDIO) y su concesión al WebView.
 *  - Mantener la pantalla encendida mientras se trabaja.
 *  - Elegir archivos de audio para importar.
 *  - Guardar el MP3/WAV exportado en Descargas/Nuby Studio.
 */
public class MainActivity extends Activity {

    private static final int REQ_PERMISSIONS = 1001;
    private static final int REQ_FILE = 1002;
    private static final String TAG = "NubyStudio";
    private static final String HOST = "localhost";
    private static final String BASE_URL = "https://" + HOST + "/";
    private static final String ENCODING = "utf-8";

    private WebView webView;
    private ValueCallback<Uri[]> fileCallback;
    private ExportSession exportSession;
    /** Petición de micrófono del WebView esperando el permiso del sistema. */
    private PermissionRequest pendingPermission;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Grabar no debe cortarse si el teléfono quiere apagar la pantalla.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        webView = new WebView(this);
        setContentView(webView);

        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setDatabaseEnabled(true);
        settings.setMediaPlaybackRequiresUserGesture(false);
        settings.setAllowFileAccess(false);
        settings.setAllowFileAccessFromFileURLs(false);
        settings.setAllowUniversalAccessFromFileURLs(false);
        settings.setAllowContentAccess(true);
        settings.setUseWideViewPort(true);
        settings.setLoadWithOverviewMode(false);
        settings.setSupportZoom(false);
        settings.setBuiltInZoomControls(false);
        settings.setTextZoom(100);
        settings.setCacheMode(WebSettings.LOAD_NO_CACHE);

        webView.setBackgroundColor(0xFF0F1214);
        webView.setWebViewClient(new LocalContentClient());
        webView.setWebChromeClient(new StudioChromeClient());
        webView.addJavascriptInterface(new NativeBridge(), "NubyNative");

        requestPermissionsIfNeeded();
        webView.loadUrl(BASE_URL + "index.html");
    }

    /**
     * El botón "atrás" del teléfono: primero cierra un diálogo abierto del estudio;
     * si no hay ninguno, deja la app en segundo plano sin perder la sesión.
     */
    @Override
    public void onBackPressed() {
        if (webView == null) {
            super.onBackPressed();
            return;
        }
        webView.evaluateJavascript(
                "(function(){"
                        + "var open=document.querySelector('dialog[open]');"
                        + "if(open){if(typeof open.close==='function'){open.close();}"
                        + "else{open.removeAttribute('open');}return 'closed';}"
                        + "return 'none';})()",
                value -> {
                    if (value == null || !value.contains("closed")) {
                        moveTaskToBack(true);
                    }
                }
        );
    }

    @Override
    protected void onDestroy() {
        if (exportSession != null) {
            exportSession.close();
            exportSession = null;
        }
        if (webView != null) {
            webView.destroy();
            webView = null;
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------------ Permisos
    private boolean hasMicrophonePermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return true;
        return checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED;
    }

    private void requestPermissionsIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return;

        List<String> missing = new ArrayList<>();
        if (!hasMicrophonePermission()) missing.add(Manifest.permission.RECORD_AUDIO);
        if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.P
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            missing.add(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        }
        if (!missing.isEmpty()) {
            requestPermissions(missing.toArray(new String[0]), REQ_PERMISSIONS);
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(code, permissions, results);
        if (code != REQ_PERMISSIONS) return;

        boolean denied = false;
        for (int index = 0; index < permissions.length; index++) {
            if (Manifest.permission.RECORD_AUDIO.equals(permissions[index])
                    && results[index] != PackageManager.PERMISSION_GRANTED) {
                denied = true;
                toast("Sin permiso de micrófono no se puede grabar. "
                        + "Actívalo en Ajustes › Aplicaciones › Nuby Studio › Permisos.");
            }
        }

        if (pendingPermission != null) {
            if (!denied && hasMicrophonePermission()) {
                pendingPermission.grant(new String[]{PermissionRequest.RESOURCE_AUDIO_CAPTURE});
            } else {
                pendingPermission.deny();
            }
            pendingPermission = null;
        }
    }

    // ------------------------------------------------------------------ Selector de archivos
    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQ_FILE) {
            if (fileCallback != null) {
                Uri[] result = null;
                if (resultCode == RESULT_OK && data != null) {
                    result = WebChromeClient.FileChooserParams.parseResult(resultCode, data);
                }
                fileCallback.onReceiveValue(result);
                fileCallback = null;
            }
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // ------------------------------------------------------------------ WebView
    /** Sirve los archivos del APK como si fueran un sitio en https://localhost. */
    private class LocalContentClient extends WebViewClient {
        @Override
        public void onPageFinished(WebView view, String url) {
            // Deja constancia en el registro de que la interfaz se cargó de verdad.
            view.evaluateJavascript(
                    "(function(){try{"
                            + "var app=document.getElementById('app');"
                            + "return (app?'NUBY-OK ':'NUBY-EMPTY ')+document.title;"
                            + "}catch(error){return 'NUBY-ERROR '+error.message;}})()",
                    value -> Log.i(TAG, "ARRANQUE " + value)
            );
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request,
                                    android.webkit.WebResourceError error) {
            if (request != null && request.isForMainFrame()) {
                Log.e(TAG, "No se pudo cargar la interfaz: " + error.getDescription());
            }
        }

        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
            Uri uri = request.getUrl();
            if (uri == null || !HOST.equals(uri.getHost())) return null;

            String path = uri.getPath();
            if (path == null || path.isEmpty() || "/".equals(path)) path = "/index.html";

            try {
                return responseFor(path, getAssets().open("www" + path));
            } catch (IOException notFound) {
                // La app es de una sola página: las rutas sin extensión vuelven al inicio.
                if (!path.contains(".")) {
                    try {
                        return responseFor("/index.html", getAssets().open("www/index.html"));
                    } catch (IOException ignored) {
                        // cae al 404
                    }
                }
                return new WebResourceResponse(
                        "text/plain",
                        ENCODING,
                        404,
                        "Not Found",
                        noCacheHeaders(),
                        new ByteArrayInputStream("404".getBytes())
                );
            }
        }

        private WebResourceResponse responseFor(String path, InputStream input) {
            return new WebResourceResponse(
                    mimeTypeFor(path),
                    isBinary(path) ? null : ENCODING,
                    200,
                    "OK",
                    noCacheHeaders(),
                    input
            );
        }
    }

    private Map<String, String> noCacheHeaders() {
        Map<String, String> headers = new HashMap<>();
        headers.put("Cache-Control", "no-store, no-cache, must-revalidate");
        headers.put("Pragma", "no-cache");
        return headers;
    }

    private boolean isBinary(String path) {
        String lower = path.toLowerCase();
        return lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg")
                || lower.endsWith(".webp") || lower.endsWith(".ico")
                || lower.endsWith(".mp3") || lower.endsWith(".wav") || lower.endsWith(".ogg");
    }

    private String mimeTypeFor(String path) {
        String lower = path.toLowerCase();
        if (lower.endsWith(".html")) return "text/html";
        if (lower.endsWith(".js") || lower.endsWith(".mjs")) return "text/javascript";
        if (lower.endsWith(".css")) return "text/css";
        if (lower.endsWith(".json")) return "application/json";
        if (lower.endsWith(".webmanifest")) return "application/manifest+json";
        if (lower.endsWith(".svg")) return "image/svg+xml";
        if (lower.endsWith(".png")) return "image/png";
        if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
        if (lower.endsWith(".webp")) return "image/webp";
        if (lower.endsWith(".mp3")) return "audio/mpeg";
        if (lower.endsWith(".wav")) return "audio/wav";
        if (lower.endsWith(".ogg")) return "audio/ogg";
        return "application/octet-stream";
    }

    /** Permite el micrófono al WebView, abre el selector de archivos y registra errores. */
    private class StudioChromeClient extends WebChromeClient {
        @Override
        public boolean onConsoleMessage(android.webkit.ConsoleMessage message) {
            if (message == null) return false;
            Log.i("NubyJS", message.messageLevel() + " " + message.message()
                    + " (" + message.sourceId() + ":" + message.lineNumber() + ")");
            return true;
        }

        @Override
        public void onPermissionRequest(final PermissionRequest request) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    List<String> grant = new ArrayList<>();
                    for (String resource : request.getResources()) {
                        if (PermissionRequest.RESOURCE_AUDIO_CAPTURE.equals(resource)) {
                            grant.add(resource);
                        }
                    }
                    if (grant.isEmpty()) {
                        request.deny();
                        return;
                    }
                    if (hasMicrophonePermission()) {
                        request.grant(grant.toArray(new String[0]));
                        return;
                    }
                    // El permiso del sistema todavía no está: se guarda la petición
                    // y se concede en cuanto el usuario acepta el aviso de Android.
                    pendingPermission = request;
                    requestPermissionsIfNeeded();
                }
            });
        }

        @Override
        public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> callback,
                                         FileChooserParams params) {
            if (fileCallback != null) {
                fileCallback.onReceiveValue(null);
                fileCallback = null;
            }
            fileCallback = callback;

            Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            if (params != null && params.getAcceptTypes() != null) {
                List<String> types = new ArrayList<>();
                for (String type : params.getAcceptTypes()) {
                    if (type != null && !type.trim().isEmpty()) types.add(type.trim());
                }
                if (!types.isEmpty()) {
                    intent.putExtra(Intent.EXTRA_MIME_TYPES, types.toArray(new String[0]));
                    if (types.contains("audio/*")) intent.setType("audio/*");
                }
            }
            try {
                startActivityForResult(Intent.createChooser(intent, "Elegir audio"), REQ_FILE);
                return true;
            } catch (Exception error) {
                fileCallback = null;
                toast("No hay una aplicación para elegir archivos");
                return false;
            }
        }
    }

    // ------------------------------------------------------------------ Puente de guardado
    /** Métodos que JavaScript ve como window.NubyNative. */
    public class NativeBridge {
        @JavascriptInterface
        public boolean start(String fileName, String mimeType) {
            try {
                if (exportSession != null) {
                    exportSession.close();
                    exportSession = null;
                }
                exportSession = new ExportSession(MainActivity.this, fileName, mimeType);
                return true;
            } catch (Exception error) {
                exportSession = null;
                return false;
            }
        }

        @JavascriptInterface
        public void chunk(String base64) {
            if (exportSession == null) return;
            try {
                exportSession.write(Base64.decode(base64, Base64.DEFAULT));
            } catch (Exception error) {
                exportSession.close();
                exportSession = null;
            }
        }

        @JavascriptInterface
        public String finish() {
            if (exportSession == null) return "";
            String location = exportSession.finish();
            exportSession = null;
            return location;
        }

        @JavascriptInterface
        public void abort() {
            if (exportSession != null) {
                exportSession.close();
                exportSession = null;
            }
        }

        @JavascriptInterface
        public void notify(String message) {
            toast(message);
        }

        @JavascriptInterface
        public boolean isAndroid() {
            return true;
        }

        @JavascriptInterface
        public boolean hasMicrophone() {
            return hasMicrophonePermission();
        }

        @JavascriptInterface
        public void requestMicrophone() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    requestPermissionsIfNeeded();
                }
            });
        }

        @JavascriptInterface
        public void openAppSettings() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    try {
                        Intent intent = new Intent(
                                android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                Uri.fromParts("package", getPackageName(), null));
                        startActivity(intent);
                    } catch (Exception error) {
                        toast("Abre Ajustes › Aplicaciones › Nuby Studio");
                    }
                }
            });
        }

        @JavascriptInterface
        public void setKeepScreenOn(final boolean enabled) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    if (enabled) {
                        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    } else {
                        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    }
                }
            });
        }
    }

    /** Escribe el archivo exportado en Descargas/Nuby Studio. */
    static class ExportSession {
        private final ContentResolver resolver;
        private final OutputStream stream;
        private final Uri uri;
        private final File file;
        private final String displayName;
        private boolean finished;

        ExportSession(Activity activity, String fileName, String mimeType) throws IOException {
            displayName = sanitize(fileName);
            resolver = activity.getContentResolver();
            String type = mimeType == null || mimeType.isEmpty() ? "audio/mpeg" : mimeType;

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, displayName);
                values.put(MediaStore.MediaColumns.MIME_TYPE, type);
                values.put(MediaStore.MediaColumns.RELATIVE_PATH,
                        Environment.DIRECTORY_DOWNLOADS + "/Nuby Studio");
                values.put(MediaStore.MediaColumns.IS_PENDING, 1);

                Uri inserted = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
                if (inserted == null) throw new IOException("Descargas rechazó el archivo");

                uri = inserted;
                file = null;
                OutputStream output = resolver.openOutputStream(inserted, "w");
                if (output == null) throw new IOException("No se pudo escribir en Descargas");
                stream = output;
            } else {
                File dir = downloadsDir(activity);
                file = new File(dir, displayName);
                uri = null;
                stream = new FileOutputStream(file);
            }
        }

        private static File downloadsDir(Activity activity) throws IOException {
            File dir = null;
            if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.P) {
                File publicDir = new File(
                        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                        "Nuby Studio");
                if (publicDir.exists() || publicDir.mkdirs()) dir = publicDir;
            }
            if (dir == null) {
                dir = new File(activity.getExternalFilesDir(Environment.DIRECTORY_MUSIC), "Nuby Studio");
                if (!dir.exists() && !dir.mkdirs()) {
                    throw new IOException("No se pudo crear la carpeta de exportación");
                }
            }
            return dir;
        }

        void write(byte[] data) throws IOException {
            if (finished) return;
            stream.write(data);
        }

        String finish() {
            if (finished) return displayName;
            finished = true;
            try {
                stream.flush();
                stream.close();
            } catch (IOException ignored) {
                // el contenido ya está escrito
            }

            if (uri != null) {
                try {
                    ContentValues values = new ContentValues();
                    values.put(MediaStore.MediaColumns.IS_PENDING, 0);
                    resolver.update(uri, values, null, null);
                } catch (Exception ignored) {
                    // el archivo queda publicado de todas formas en la mayoría de equipos
                }
                return "Descargas/Nuby Studio/" + displayName;
            }
            return file == null ? displayName : "Descargas/Nuby Studio/" + displayName;
        }

        void close() {
            if (finished) return;
            finished = true;
            try {
                stream.flush();
                stream.close();
            } catch (IOException ignored) {
                // nada que hacer
            }
            if (uri != null) {
                try {
                    resolver.delete(uri, null, null);
                } catch (Exception ignored) {
                    // nada que hacer
                }
            } else if (file != null) {
                //noinspection ResultOfMethodCallIgnored
                file.delete();
            }
        }

        private static String sanitize(String name) {
            String clean = name == null ? "nuby-export" : name.trim();
            clean = clean.replaceAll("[\\\\/:*?\"<>|]", "-");
            if (clean.isEmpty()) clean = "nuby-export";
            return clean.length() > 120 ? clean.substring(clean.length() - 120) : clean;
        }
    }

    private void toast(final String message) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(MainActivity.this, message, Toast.LENGTH_LONG).show();
            }
        });
    }
}
