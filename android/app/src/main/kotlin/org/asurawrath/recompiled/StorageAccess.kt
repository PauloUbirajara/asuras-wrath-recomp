package org.asurawrath.recompiled

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.Settings
import java.io.File

/**
 * The game opens its files by path, so a folder the player picks (a
 * content:// tree from the system picker) is turned into its path, and the
 * app needs "All files access" (Android 11+) or the storage permissions
 * (Android 10) to read it.
 */
object StorageAccess {
    const val LEGACY_REQUEST = 41

    fun granted(activity: Activity): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            activity.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
                PackageManager.PERMISSION_GRANTED
        }

    /** Opens the system's page (or dialog, Android 10) to grant it. */
    fun request(activity: Activity) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val intent = Intent(
                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:${activity.packageName}"),
            )
            try {
                activity.startActivity(intent)
            } catch (e: Exception) {
                activity.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        } else {
            activity.requestPermissions(
                arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE),
                LEGACY_REQUEST,
            )
        }
    }

    /**
     * The path of a folder from the system picker: internal storage
     * ("primary:Games/Asura") or an SD card ("1A2B-3C4D:Games/Asura").
     * Null for other providers (Downloads, cloud drives).
     */
    fun treePath(uri: Uri): File? {
        if (uri.authority != "com.android.externalstorage.documents") return null
        val id = DocumentsContract.getTreeDocumentId(uri) ?: return null
        val volume = id.substringBefore(':')
        val relative = id.substringAfter(':', "")
        val base = if (volume.equals("primary", ignoreCase = true)) {
            Environment.getExternalStorageDirectory()
        } else {
            File("/storage/$volume")
        }
        return if (relative.isEmpty()) base else File(base, relative)
    }
}
