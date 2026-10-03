package com.example.pythonandroid;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * 把 assets 中的 Python 运行时 / 项目解压到 App 私有目录。
 *
 * assets 布局（由 scripts/prepare.py 组装）：
 *   assets/python/<abi>/lib/python3.14/...   官方标准库（按 ABI 分开，lib-dynload 是 .so）
 *   assets/site-packages/...                 用户 Python 库（各 ABI 共享）
 *   assets/wheels/...                        pip / setuptools 离线 wheel
 *   assets/project/...                       用户的 Python 项目（启动即运行 main.py）
 *
 * 说明：
 *  - 官方 testbed 用“文件名末尾加 '-'”规避 AAPT 对 .gz 资产的特殊处理，
 *    解压时把末尾的 '-' 还原（仅对 python/ 树生效，不污染用户项目）。
 *  - 标准库只解压一次（.complete 标记）；项目在 VERSION 变化时重新解压。
 */
public final class AssetExtractor {

    private static final String TAG = "AssetExtractor";

    private AssetExtractor() {
    }

    /** 解压标准库到 filesDir/python（按设备 ABI）。返回 pythonHome。 */
    public static File extractPython(Context ctx, String abi) throws IOException {
        File home = new File(ctx.getFilesDir(), "python");
        File marker = new File(home, ".complete");

        // 选本设备 ABI（assets 里存在即用，否则按候选顺序找）
        String[] candidates = {"arm64-v8a", "x86_64", "armeabi-v7a", "x86"};
        String srcAbi = abi;
        for (String c : candidates) {
            if (srcAbi.equals(c) && assetDirExists(ctx, "python/" + c + "/lib")) {
                srcAbi = c;
                break;
            }
            srcAbi = c;
            if (assetDirExists(ctx, "python/" + c + "/lib")) {
                break;
            }
        }
        Log.i(TAG, "extract stdlib for abi=" + srcAbi);

        // 期望文件数：assets 里 common + 本 ABI + site-packages 的文件总数
        // （与下方解压的拷贝范围一一对应；lib-dynload 已含在 <abi>/lib 内）
        int expected = countAssetFiles(ctx, "python/common/lib")
                + countAssetFiles(ctx, "python/" + srcAbi + "/lib")
                + countAssetFiles(ctx, "site-packages")
                + countAssetFiles(ctx, "python/certs");

        if (marker.exists()) {
            int recorded = readMarker(marker);
            if (isPythonUsable(home) && recorded == expected) {
                return home;
            }
            // 标记存在但内容不完整（旧版覆盖安装、解压中断等）：
            // 强制删除重建，避免一直用残缺标准库。
            // 典型症状：Py_InitializeFromConfig failed: Failed to import encodings module
            Log.w(TAG, "已解压内容不完整（文件数 " + recorded + "/" + expected
                    + "），强制重新解压");
            deleteRecursively(home);
        }

        // 1) 标准库：纯 Python 部分共享（common）+ 本 ABI 的 lib-dynload（C 扩展）
        //    计数与上方 expected 对应；wheels 拷贝到 filesDir 根，不参与计数
        java.util.concurrent.atomic.AtomicInteger copied = new java.util.concurrent.atomic.AtomicInteger();
        copyAssetTree(ctx, "python/common/lib", new File(home, "lib"), true, copied);
        copyAssetTree(ctx, "python/" + srcAbi + "/lib", new File(home, "lib"), true, copied);
        // 2) 共享 site-packages
        copyAssetTree(ctx, "site-packages", new File(home, "lib/python3.14/site-packages"), false, copied);
        // 2.5) 内置根证书（Let's Encrypt ISRG X1/X2，Android 10 系统 CA 缺 X2）
        copyAssetTree(ctx, "python/certs", new File(home, "certs"), false, copied);
        // 3) wheels（pip 离线引导用）
        copyAssetTree(ctx, "wheels", new File(ctx.getFilesDir(), "wheels"), false, null);

        // 空目录在 asset 打包时丢失，手动补 cwd
        new File(home, "cwd").mkdirs();
        // marker 记录实际拷贝文件数，供下次启动做完整性比对
        Files.write(marker.toPath(), String.valueOf(copied.get()).getBytes(StandardCharsets.UTF_8));
        return home;
    }

    /** 解压项目到 filesDir/project；VERSION 变化时重新解压。 */
    public static File extractProject(Context ctx) throws IOException {
        File projectDir = new File(ctx.getFilesDir(), "project");

        String newVer = readAssetString(ctx, "project/VERSION");
        if (newVer == null) newVer = "0";
        File verFile = new File(projectDir, ".version");
        String oldVer = verFile.exists()
                ? new String(Files.readAllBytes(verFile.toPath()), StandardCharsets.UTF_8).trim()
                : null;

        if (projectDir.exists() && newVer.equals(oldVer)) {
            return projectDir;
        }

        // 重新解压（先清理旧目录，保留用户自己的运行期产物？——项目目录是受管的，直接重建）
        deleteRecursively(projectDir);
        copyAssetTree(ctx, "project", projectDir, false, null);
        Files.write(verFile.toPath(), newVer.getBytes(StandardCharsets.UTF_8));
        return projectDir;
    }

    /* ---------------- 工具方法 ---------------- */

    /** 快速自检：已解压的标准库是否真的可用（encodings 是 Python 启动的硬依赖）。 */
    private static boolean isPythonUsable(File home) {
        return new File(home, "lib/python3.14/encodings/aliases.py").exists()
                && new File(home, "lib/python3.14/os.py").exists();
    }

    private static boolean assetDirExists(Context ctx, String path) {
        try {
            return ctx.getAssets().list(path) != null;
        } catch (IOException e) {
            return false;
        }
    }

    private static String readAssetString(Context ctx, String path) {
        try (InputStream in = ctx.getAssets().open(path)) {
            byte[] buf = new byte[in.available()];
            int n = 0;
            while (n < buf.length) {
                int r = in.read(buf, n, buf.length - n);
                if (r < 0) break;
                n += r;
            }
            return new String(buf, 0, n, StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * 递归拷贝 assets 目录到磁盘。
     *
     * @param undoGzDash true 时把文件名末尾的 '-' 还原（官方 .gz 规避技巧）
     * @param counter    非 null 时每拷一个文件 +1（用于完整性校验计数）
     */
    private static void copyAssetTree(Context ctx, String assetPath, File targetDir,
                                      boolean undoGzDash,
                                      java.util.concurrent.atomic.AtomicInteger counter)
            throws IOException {
        String[] names = ctx.getAssets().list(assetPath);
        if (names == null || names.length == 0) {
            return;
        }
        if (!targetDir.exists() && !targetDir.mkdirs()) {
            throw new IOException("无法创建目录: " + targetDir);
        }
        for (String name : names) {
            String sub = assetPath + "/" + name;
            InputStream in;
            try {
                in = ctx.getAssets().open(sub);
            } catch (IOException e) {
                // 是子目录：递归到子目录下（必须把子目录名加进 targetDir，
                // 否则子目录内容会被平铺到父目录，导致 encodings/ 等包目录丢失）
                File subDir = new File(targetDir, name);
                copyAssetTree(ctx, sub, subDir, undoGzDash, counter);
                continue;
            }
            String outName = name;
            if (undoGzDash) {
                // 官方 testbed：AAPT 打包时会特殊处理 .gz，prepare.py 已给相关文件加 '-'
                if (outName.endsWith("-")) {
                    outName = outName.substring(0, outName.length() - 1);
                }
            }
            File out = new File(targetDir, outName);
            try (InputStream is = in; FileOutputStream fos = new FileOutputStream(out)) {
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = is.read(buf)) > 0) {
                    fos.write(buf, 0, n);
                }
            }
            if (counter != null) {
                counter.incrementAndGet();
            }
        }
    }

    /** 统计 assets 目录树下的文件总数（与解压计数同口径：list() 非空=目录）。 */
    private static int countAssetFiles(Context ctx, String path) {
        int n = 0;
        try {
            String[] names = ctx.getAssets().list(path);
            if (names == null) {
                return 0;
            }
            for (String name : names) {
                String child = path + "/" + name;
                String[] sub = ctx.getAssets().list(child);
                if (sub != null && sub.length > 0) {
                    n += countAssetFiles(ctx, child);
                } else {
                    n += 1;
                }
            }
        } catch (IOException e) {
            // 忽略：路径不存在按 0 计
        }
        return n;
    }

    /** 读取 marker 里记录的解压文件数；非法内容返回 -1。 */
    private static int readMarker(File marker) {
        try {
            return Integer.parseInt(new String(Files.readAllBytes(marker.toPath()),
                    StandardCharsets.UTF_8).trim());
        } catch (Exception e) {
            return -1;
        }
    }

    private static void deleteRecursively(File f) {
        if (f == null || !f.exists()) return;
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) deleteRecursively(c);
        }
        f.delete();
    }
}
