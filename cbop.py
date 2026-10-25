#!/usr/bin/env python3
"""
Собирает файлы из папок kernel/src и kernel/include в txt-файлы,
разбивая содержимое так, чтобы в каждом txt было не более 2500 строк.

Файлы из EXCLUDE_FILENAMES пропускаются и НЕ записываются в дампы.
Папки из EXCLUDE_DIRS не обходятся и НЕ записываются в дампы.
"""

import os
import sys
from pathlib import Path
from fnmatch import fnmatch

# ---------- Настройки ----------
SOURCE_DIRS = ["kernel/src", "kernel/include"]
OUTPUT_DIR = "kernel_dump"
MAX_LINES = 2500

# Расширения файлов, которые нужно собирать (None = все файлы)
INCLUDE_EXTENSIONS = None  # например: {".c", ".h", ".cpp", ".hpp", ".s", ".asm"}

# ---------- Исключения (НЕ записывать в дамп) ----------
# Точные имена файлов, регистронезависимо
EXCLUDE_FILENAMES = {
    "basefont.h",
    "usb_data.h",
}

# Дополнительно — по маске (glob), регистронезависимо.
# Например: {"*_generated.*", "*.bak"}
EXCLUDE_PATTERNS = set()

# Исключать по фрагменту относительного пути (регистронезависимо).
# Например: {"kernel/src/vendor/", "/third_party/"}
EXCLUDE_PATH_PARTS = set()

# Папки, которые НЕ нужно обходить и записывать в дамп.
# Можно указывать:
#   - относительный путь от корня проекта: "kernel/src/vendor"
#   - просто имя папки: "vendor" — исключит любую папку с таким именем
EXCLUDE_DIRS = {
    "kernel/src/fs/lwext4",
    "kernel/include/fs/lwext4"
}

FILE_HEADER = "=" * 80
SECTION_HEADER = "-" * 80


def is_excluded_dir(path: Path) -> bool:
    """True — если папку нужно пропустить (не обходить)."""
    p_norm = str(path).lower().replace("\\", "/").strip("/")
    parts = p_norm.split("/")

    for d in EXCLUDE_DIRS:
        d_norm = d.lower().replace("\\", "/").strip("/")
        if not d_norm:
            continue
        # Полный путь или путь внутри исключённой папки
        if p_norm == d_norm or p_norm.startswith(d_norm + "/"):
            return True
        # Если указано просто имя папки — ищем его в любом месте пути
        if "/" not in d_norm and d_norm in parts:
            return True
    return False


def is_excluded(path: Path) -> bool:
    """True — если файл нужно пропустить (не записывать в дамп)."""
    name_lower = path.name.lower()
    path_str_lower = str(path).lower().replace("\\", "/")
    parts = path_str_lower.strip("/").split("/")

    if name_lower in {n.lower() for n in EXCLUDE_FILENAMES}:
        return True

    for pattern in EXCLUDE_PATTERNS:
        if fnmatch(name_lower, pattern.lower()):
            return True

    for part in EXCLUDE_PATH_PARTS:
        if part.lower().replace("\\", "/") in path_str_lower:
            return True

    # Дополнительно проверяем, не лежит ли файл в исключённой папке
    for d in EXCLUDE_DIRS:
        d_norm = d.lower().replace("\\", "/").strip("/")
        if not d_norm:
            continue
        if path_str_lower == d_norm or path_str_lower.startswith(d_norm + "/"):
            return True
        if "/" not in d_norm and d_norm in parts:
            return True

    return False


def collect_files(source_dirs):
    """Собирает отсортированный список файлов, пропуская исключённые."""
    files = []
    excluded_names = []
    for src in source_dirs:
        src_path = Path(src)
        if not src_path.exists():
            print(f"[!] Папка не найдена: {src}", file=sys.stderr)
            continue

        # os.walk позволяет пропускать целые папки, не обходя их
        for root, dirs, filenames in os.walk(src_path):
            root_path = Path(root)

            # Пропускаем исключённые папки
            dirs[:] = [d for d in dirs if not is_excluded_dir(root_path / d)]

            for name in filenames:
                path = root_path / name
                if INCLUDE_EXTENSIONS is not None:
                    if path.suffix.lower() not in INCLUDE_EXTENSIONS:
                        continue
                if is_excluded(path):
                    excluded_names.append(str(path))
                    continue
                files.append(path)

    files.sort(key=lambda p: str(p).lower())
    return files, excluded_names


def read_file_lines(path):
    """Читает файл, возвращает список строк (с добавленным переводом строки)."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
    except Exception as e:
        print(f"[!] Не удалось прочитать {path}: {e}", file=sys.stderr)
        return [f"<< Ошибка чтения файла: {e} >>\n"]

    content = content.replace("\r\n", "\n").replace("\r", "\n")
    lines = content.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return [line + "\n" for line in lines]


def build_blocks(files, max_lines):
    """Генерирует блоки строк (<= max_lines) для записи в отдельные txt."""
    current_block = []
    current_count = 0

    for path in files:
        file_lines = read_file_lines(path)

        header = [
            SECTION_HEADER + "\n",
            f"FILE: {path}\n",
            SECTION_HEADER + "\n",
        ]
        chunk = header + file_lines + ["\n"]

        if current_count + len(chunk) > max_lines and current_block:
            yield current_block
            current_block = []
            current_count = 0

        if len(chunk) > max_lines:
            if current_block:
                yield current_block
                current_block = []
                current_count = 0

            idx = 0
            while idx < len(chunk):
                space = max_lines - current_count
                take = chunk[idx: idx + space]
                current_block.extend(take)
                current_count += len(take)
                idx += len(take)

                if current_count >= max_lines:
                    yield current_block
                    current_block = []
                    current_count = 0
        else:
            current_block.extend(chunk)
            current_count += len(chunk)
            if current_count >= max_lines:
                yield current_block
                current_block = []
                current_count = 0

    if current_block:
        yield current_block


def main():
    files, excluded = collect_files(SOURCE_DIRS)

    if excluded:
        print(f"[i] Пропущено (не записываем): {len(excluded)}")
        for name in excluded:
            print(f"    - {name}")

    if not files:
        print("[!] Не найдено ни одного файла для обработки.")
        return

    print(f"[+] Файлов к обработке: {len(files)}")

    out_dir = Path(OUTPUT_DIR)
    out_dir.mkdir(parents=True, exist_ok=True)

    total_lines = 0
    part = 0
    for block in build_blocks(files, MAX_LINES):
        part += 1
        out_file = out_dir / f"kernel_dump_part{part:03d}.txt"
        with open(out_file, "w", encoding="utf-8") as f:
            f.writelines(block)
        lines_in_block = len(block)
        total_lines += lines_in_block
        print(f"    -> {out_file}  ({lines_in_block} строк)")

    print(f"\n[✓] Готово. Файлов: {part}, всего строк: {total_lines}")
    print(f"[✓] Результат в папке: {out_dir.resolve()}")


if __name__ == "__main__":
    main()