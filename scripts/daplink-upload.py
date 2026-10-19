import struct
import subprocess
from pathlib import Path

Import("env")

# This generic board omits CMSIS-DAP from its declared upload protocols.
board = env.BoardConfig()
board.update("debug.tools.cmsis-dap", {
    "server": {
        "arguments": [
            "-s", "$PACKAGE_DIR/openocd/scripts",
            "-f", "interface/cmsis-dap.cfg",
            "-c", "transport select swd",
            "-f", "target/stm32f4x.cfg",
        ]
    }
})


def prepare_metadata(source, target, env):
    build_dir = Path(env.subst("$BUILD_DIR")).resolve()
    firmware = (build_dir / env.subst("${PROGNAME}.bin")).read_bytes()
    if not 0 < len(firmware) <= 448 * 1024:
        raise ValueError("Application must fit in 0x08010000-0x0807FFFF")

    # Match Calculate_CRC32(): little-endian words, ignoring a partial final word.
    crc = 0xFFFFFFFF
    for (word,) in struct.iter_unpack("<I", firmware[:len(firmware) // 4 * 4]):
        crc ^= word
        for _ in range(32):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF

    backup = build_dir / "upgrade-info-before-upload.bin"
    metadata = build_dir / "upgrade-info.bin"
    flags = list(env["UPLOADERFLAGS"])
    # Keep PlatformIO's probe, script path, and speed settings; replace program.
    connection_flags = flags[:-2]
    subprocess.run([
        env.subst("$UPLOADER"), *connection_flags,
        "-c", "init",
        "-c", f"dump_image {{{backup.as_posix()}}} 0x0800C000 16384",
        "-c", "shutdown",
    ], check=True, timeout=30)
    sector = bytearray(backup.read_bytes())
    if len(sector) != 16384:
        raise ValueError("Incomplete upgrade metadata sector backup")
    sector[:16] = struct.pack("<IIII", 0xDEADBEEF, 0x5A5A5A5A, len(firmware), crc)
    metadata.write_bytes(sector)
    print(f"Application CRC32: 0x{crc:08X}; metadata backup: {backup}")
    env.Replace(UPLOADERFLAGS=[
        *connection_flags,
        "-c", "program {$SOURCE} 0x08010000 verify; "
              f"program {{{metadata.as_posix()}}} 0x0800C000 verify reset; shutdown;",
    ])


if env.subst("$PIOENV") == "power_pico" and env.GetProjectOption("upload_protocol") == "cmsis-dap":
    env.AddPreAction("upload", prepare_metadata)
