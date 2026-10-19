"""Run with: python tests/check_daplink_upload.py"""
import contextlib
import io
import runpy
import struct
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


class UploadEnv(dict):
    def __init__(self, build_dir):
        super().__init__(UPLOADERFLAGS=["-f", "target/stm32f4x.cfg", "-c", "program old"])
        self.build_dir = str(build_dir)

    def subst(self, value):
        return {"$PIOENV": "power_pico", "$BUILD_DIR": self.build_dir,
                "${PROGNAME}.bin": "firmware.bin", "$UPLOADER": "openocd"}[value]

    def BoardConfig(self):
        return SimpleNamespace(update=lambda key, value: None)

    def GetProjectOption(self, name):
        return "cmsis-dap"

    def AddPreAction(self, target, action):
        self.prepare_metadata = action

    def Replace(self, **values):
        self.update(values)


with tempfile.TemporaryDirectory() as directory:
    build = Path(directory)
    env = UploadEnv(build)
    script = Path(__file__).resolve().parents[1] / "scripts/daplink-upload.py"
    runpy.run_path(str(script), init_globals={"env": env, "Import": lambda name: None})
    original = bytes(range(256)) * 64

    def read_flash(*args, **kwargs):
        (build / "upgrade-info-before-upload.bin").write_bytes(original)

    # STM32 CRC32 of one zero word; bootloader ignores trailing 1-3 bytes.
    for tail in (b"", b"\x12", b"\x12\x34", b"\x12\x34\x56"):
        firmware = b"\0" * 4 + tail
        (build / "firmware.bin").write_bytes(firmware)
        with patch("subprocess.run", side_effect=read_flash), contextlib.redirect_stdout(io.StringIO()):
            env.prepare_metadata([], [], env)
        result = (build / "upgrade-info.bin").read_bytes()
        assert struct.unpack("<IIII", result[:16]) == (
            0xDEADBEEF, 0x5A5A5A5A, len(firmware), 0xC704DD7B)
        assert result[16:] == original[16:], "Unrelated metadata-sector data changed"
        assert (build / "upgrade-info-before-upload.bin").read_bytes() == original

print("PASS: STM32 CRC word order, partial-word handling, and metadata-sector preservation")
