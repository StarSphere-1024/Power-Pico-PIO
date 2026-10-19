"""Run with: python tests/check_ui_fonts.py (requires CMake and a native C compiler)."""
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="power-pico-font-check-") as directory:
    build = Path(directory)
    (build / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.16)
project(ui_font_check C)
set(CMAKE_C_STANDARD 11)
file(GLOB_RECURSE LVGL_SOURCES "{ROOT}/lib/LVGL/src/*.c")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "/drivers/")
file(GLOB FONT_SOURCES "{ROOT}/src/app/User/GUI/font/*.c")
add_executable(check_ui_fonts ${{LVGL_SOURCES}} ${{FONT_SOURCES}}
    "{ROOT}/src/app/User/GUI/common/lv_i18n.c" "{ROOT}/tests/check_ui_fonts.c")
target_include_directories(check_ui_fonts PRIVATE "{ROOT}/lib/LVGL"
    "{ROOT}/src/app/User/GUI")
target_compile_definitions(check_ui_fonts PRIVATE LV_CONF_INCLUDE_SIMPLE)
target_compile_options(check_ui_fonts PRIVATE -Os -ffunction-sections -fdata-sections)
target_link_options(check_ui_fonts PRIVATE -Wl,--gc-sections)
target_link_libraries(check_ui_fonts PRIVATE m)
""", encoding="utf-8")
    subprocess.run(["cmake", "-S", directory, "-B", str(build / "build")], check=True)
    subprocess.run(["cmake", "--build", str(build / "build"), "-j", "4"], check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run([str(build / "build/check_ui_fonts")], check=True)
