package jp.digimonbc.port;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/**
 * Java mirror of settings_check_disc() from the native player. The launcher can
 * therefore tell the user whether the imported image is the exact disc this build
 * was generated from, without starting SDL.
 */
final class DiscImage {
    enum Status {
        OK,
        COMPRESSED,
        TOO_SMALL,
        NOT_GAMECUBE,
        WRONG_GAME,
        WRONG_REV,
        DOL_MISMATCH,
        IO_ERROR
    }

    static final class Result {
        final Status status;
        final long size;
        final String gameId;
        final String gameName;
        final int revision;

        Result(Status status, long size, String gameId, String gameName, int revision) {
            this.status = status;
            this.size = size;
            this.gameId = gameId == null ? "" : gameId;
            this.gameName = gameName == null ? "" : gameName;
            this.revision = revision;
        }

        boolean ok() {
            return status == Status.OK;
        }
    }

    private static final String GAME_ID = "G2DJB2";
    private static final long GAME_DOL_SIZE = 3591200L;
    private static final long GAME_DOL_FNV = 0x5FD8E0C4B410A2D2L;
    private static final Set<String> COMPRESSED = new HashSet<>(Arrays.asList(
            ".rvz", ".wia", ".gcz", ".ciso", ".wbfs", ".nfs"));

    private DiscImage() {
    }

    static String extension(String name) {
        if (name == null) return "";
        int dot = name.lastIndexOf('.');
        return dot < 0 ? "" : name.substring(dot).toLowerCase(Locale.ROOT);
    }

    static Result check(File file) {
        if (file == null || !file.isFile() || !file.canRead())
            return new Result(Status.IO_ERROR, 0, "", "", 0);
        if (COMPRESSED.contains(extension(file.getName())))
            return new Result(Status.COMPRESSED, file.length(), "", "", 0);

        long length = file.length();
        if (length < 0x440)
            return new Result(Status.TOO_SMALL, length, "", "", 0);

        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            byte[] header = new byte[0x440];
            raf.readFully(header);
            if ((header[0x1C] & 0xFF) != 0xC2 || (header[0x1D] & 0xFF) != 0x33 ||
                    (header[0x1E] & 0xFF) != 0x9F || (header[0x1F] & 0xFF) != 0x3D)
                return new Result(Status.NOT_GAMECUBE, length, "", "", 0);

            String id = new String(header, 0, 6, java.nio.charset.StandardCharsets.US_ASCII);
            String name = readName(header);
            int revision = header[7] & 0xFF;
            if (!GAME_ID.equals(id))
                return new Result(Status.WRONG_GAME, length, id, name, revision);
            if (revision != 0)
                return new Result(Status.WRONG_REV, length, id, name, revision);

            long dolOffset = be32(header, 0x420) & 0xFFFFFFFFL;
            if (dolOffset <= 0 || dolOffset + 0x100 > length)
                return new Result(Status.NOT_GAMECUBE, length, id, name, revision);

            byte[] dolHeader = new byte[0x100];
            raf.seek(dolOffset);
            raf.readFully(dolHeader);
            long dolSize = 0x100;
            for (int i = 0; i < 18; i++) {
                long end = (be32(dolHeader, i * 4) & 0xFFFFFFFFL) +
                        (be32(dolHeader, 0x90 + i * 4) & 0xFFFFFFFFL);
                if ((be32(dolHeader, 0x90 + i * 4) & 0xFFFFFFFFL) != 0 && end > dolSize)
                    dolSize = end;
            }
            if (dolSize > 0x01800000L || dolOffset + dolSize > length)
                return new Result(Status.DOL_MISMATCH, length, id, name, revision);

            byte[] dol = new byte[(int) dolSize];
            raf.seek(dolOffset);
            raf.readFully(dol);
            long fnv = 0xcbf29ce484222325L;
            for (byte b : dol) {
                fnv ^= (b & 0xFF);
                fnv *= 0x100000001B3L;
            }
            if (dolSize != GAME_DOL_SIZE || fnv != GAME_DOL_FNV)
                return new Result(Status.DOL_MISMATCH, length, id, name, revision);
            return new Result(Status.OK, length, id, name, revision);
        } catch (IOException e) {
            return new Result(Status.IO_ERROR, length, "", "", 0);
        }
    }

    private static String readName(byte[] header) {
        byte[] raw = Arrays.copyOfRange(header, 0x20, 0x40);
        StringBuilder text = new StringBuilder(32);
        for (byte b : raw) {
            int c = b & 0xFF;
            text.append(c >= 0x20 && c < 0x7F ? (char) c : c == 0 ? '\0' : '?');
        }
        int end = text.indexOf("\0");
        if (end >= 0) text.setLength(end);
        return text.toString();
    }

    private static int be32(byte[] data, int offset) {
        return ((data[offset] & 0xFF) << 24) | ((data[offset + 1] & 0xFF) << 16) |
                ((data[offset + 2] & 0xFF) << 8) | (data[offset + 3] & 0xFF);
    }
}
