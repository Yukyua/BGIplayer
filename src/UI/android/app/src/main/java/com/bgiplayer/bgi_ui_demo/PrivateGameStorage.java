package com.bgiplayer.bgi_ui_demo;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;

/** Restricts destructive operations to one imported private game, never its SAF source. */
public final class PrivateGameStorage {
    private PrivateGameStorage() {}

    public static File resolve(File filesDir, String path, String activePath) throws IOException {
        if (path == null || path.isEmpty()) throw new IOException("缺少游戏副本路径");
        File root = new File(filesDir, "Games").getCanonicalFile();
        File requested = new File(path).getAbsoluteFile();
        File game = requested.getCanonicalFile();
        if (requested.getParentFile() == null || !requested.toPath().normalize().equals(requested.toPath()) ||
            !root.equals(requested.getParentFile().getCanonicalFile()) ||
            !root.equals(game.getParentFile()) ||
            !game.isDirectory() || Files.isSymbolicLink(requested.toPath())) {
            throw new IOException("只能删除应用 Games 目录内的游戏副本");
        }
        if (activePath != null && game.equals(new File(activePath).getCanonicalFile())) {
            throw new IOException("请先退出正在运行的游戏");
        }
        return game;
    }

    public static void remove(File filesDir, String path, String activePath) throws IOException {
        removeTree(resolve(filesDir, path, activePath));
    }

    private static void removeTree(File file) throws IOException {
        // A link inside a game is unlinked, never followed to an external directory.
        if (!Files.isSymbolicLink(file.toPath()) && file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) throw new IOException("无法读取游戏副本目录");
            for (File child : children) removeTree(child);
        }
        if (!file.delete()) throw new IOException("无法完整删除游戏副本，请重试");
    }
}
