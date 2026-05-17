"""Audio Converter - Batch convert audio files."""
import os
import shutil
import threading
import time
from concurrent.futures import ThreadPoolExecutor, wait
from tkinter import filedialog, ttk, messagebox

from mutagen import File as MutagenFile
from mutagen.flac import FLAC, Picture
from mutagen.id3 import APIC
from mutagen.mp4 import MP4Cover

import customtkinter as ctk
try:
    from customtkinter.windows.widgets.ctk_tooltip import CTkToolTip
except ImportError:
    CTkToolTip = None

import miniaudio
import numpy as np
import soundfile as sf

from ncm_decryptor import decrypt_ncm

T = {
    "title":            {"zh": "\u266b 音频转换工具",       "en": "\u266b Audio Converter"},
    "source_folder":    {"zh": "源文件夹",                 "en": "Source folder"},
    "source_ph":        {"zh": "选择包含音乐文件的文件夹…", "en": "Select folder containing music files\u2026"},
    "browse":           {"zh": "浏览\u2026",               "en": "Browse\u2026"},
    "file_name":        {"zh": "文件名",                   "en": "File name"},
    "format":           {"zh": "格式",                     "en": "Format"},
    "size":             {"zh": "大小",                     "en": "Size"},
    "output_folder":    {"zh": "输出文件夹",               "en": "Output folder"},
    "output_ph":        {"zh": "选择目标文件夹…",          "en": "Select destination folder\u2026"},
    "output_fmt":       {"zh": "输出格式",                 "en": "Output format"},
    "start":            {"zh": "开始转换",                 "en": "Start Conversion"},
    "converting":       {"zh": "转换中\u2026",            "en": "Converting\u2026"},
    "ready":            {"zh": "就绪",                     "en": "Ready"},
    "preparing":        {"zh": "准备中\u2026",            "en": "Preparing\u2026"},
    "no_files":         {"zh": "没有可转换的文件",         "en": "No files to convert."},
    "no_output":        {"zh": "请先选择输出文件夹",       "en": "Select an output folder first."},
    "info":             {"zh": "提示",                     "en": "Info"},
    "files_count":      {"zh": "{} 个文件",               "en": "{} files"},
    "progress_fmt":     {"zh": "正在转换 ({}/{})",        "en": "Converting ({}/{})"},
    "done_title":       {"zh": "转换完成",                 "en": "Complete"},
    "done_fmt":         {"zh": "成功: {}  失败: {}\n\n{}",  "en": "Success: {}  Failed: {}\n\n{}"},
    "failed_prefix":    {"zh": "失败: ",                   "en": "Failed: "},
    "cancelled":        {"zh": "已取消",                   "en": "Cancelled"},
    "scan_error":       {"zh": "扫描失败: {}",             "en": "Scan failed: {}"},
    "theme_tip":        {"zh": "切换深色/浅色模式",       "en": "Toggle dark / light mode"},
    "lang_switch":      {"zh": "EN",                       "en": "\u4e2d"},
}

SUPPORTED_EXT = {".mp3", ".flac", ".m4a", ".ncm"}
OUTPUT_FORMATS = ["FLAC", "WAV"]


def _remove(path):
    if path and os.path.exists(path):
        try:
            os.remove(path)
        except OSError:
            pass


def _friendly_error(e: Exception) -> str:
    msg = str(e).lower()
    if "decode" in msg or "unexpected" in msg or "corrupt" in msg:
        return "音频解码失败"
    if "write" in msg or "permission" in msg:
        return "写入文件失败"
    if "unsupported" in msg:
        return "不支持的格式"
    if "decrypt" in msg or "ncm" in msg:
        return "NCM解密失败"
    s = str(e)
    return s[:80] if len(s) > 80 else s


def _copy_metadata(source: str, dest: str):
    """Copy audio tags and cover art. Falls back to raw binary scan on failure."""
    tags = {}
    try:
        src = MutagenFile(source)
        if src is not None and src.tags:
            tags = dict(src.tags)
    except Exception:
        pass

    pictures = []
    try:
        src = MutagenFile(source)
        if src is not None:
            if hasattr(src, 'pictures'):
                pictures.extend(src.pictures)
            if hasattr(src, 'tags') and src.tags:
                if hasattr(src.tags, 'getall'):
                    for apic in src.tags.getall('APIC'):
                        pic = Picture()
                        pic.data = apic.data
                        pic.type = apic.type
                        pic.mime = apic.mime
                        pic.desc = apic.desc
                        pictures.append(pic)
                if hasattr(src.tags, 'get'):
                    for key in ('\xa9covr', 'covr'):
                        covr_list = src.tags.get(key)
                        if covr_list:
                            for cover in covr_list:
                                pic = Picture()
                                pic.data = bytes(cover)
                                pic.type = 3
                                pic.mime = 'image/jpeg' if cover.imageformat == MP4Cover.FORMAT_JPEG else 'image/png'
                                pictures.append(pic)
                            break
    except Exception:
        pass

    if not pictures:
        try:
            with open(source, "rb") as f:
                raw = f.read(128 * 1024)
            idx = raw.find(b'\xff\xd8')
            if idx >= 0:
                end = raw.find(b'\xff\xd9', idx + 2)
                if end >= 0:
                    pic = Picture()
                    pic.data = raw[idx:end + 2]
                    pic.type = 3
                    pic.mime = 'image/jpeg'
                    pictures.append(pic)
        except Exception:
            pass

    if not tags and not pictures:
        return

    try:
        dst = FLAC(dest)
        for key, value in tags.items():
            dst[key] = value
        for pic in pictures:
            dst.add_picture(pic)
        dst.save()
    except Exception:
        pass


class AudioConverterApp(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Audio Converter")
        self.geometry("780x680")
        self.minsize(660, 560)
        ctk.set_default_color_theme("green")
        ctk.set_appearance_mode("light")
        self._lang = "zh"
        self._audio_files: list[dict] = []
        self._input_dir = ""
        self._output_dir = ""
        self._out_fmt = "FLAC"
        self._stop_flag = False
        self._theme_animating = False
        self._widget_texts: dict = {}
        self._build_ui()
        self._apply_language()
        self._update_theme_icon()

    def _t(self, key: str) -> str:
        return T[key][self._lang]

    @staticmethod
    def _fmt_size(num_bytes: int) -> str:
        if num_bytes < 1024:
            return f"{num_bytes} B"
        if num_bytes < 1024 * 1024:
            return f"{num_bytes / 1024:.1f} KB"
        return f"{num_bytes / (1024 * 1024):.1f} MB"

    def _build_ui(self):
        outer = ctk.CTkFrame(self, fg_color="transparent")
        outer.pack(fill="both", expand=True, padx=28, pady=20)

        title_row = ctk.CTkFrame(outer, fg_color="transparent")
        title_row.pack(fill="x", pady=(0, 12))
        self._title_label = ctk.CTkLabel(title_row, text="", font=ctk.CTkFont(size=20, weight="bold"))
        self._title_label.pack(side="left")
        self._reg_widget(self._title_label, "title")

        btn_frame = ctk.CTkFrame(title_row, fg_color="transparent")
        btn_frame.pack(side="right")
        self._lang_btn = ctk.CTkButton(btn_frame, text="", width=40, height=36, command=self._toggle_language)
        self._lang_btn.pack(side="right", padx=(8, 0))
        self._reg_widget(self._lang_btn, "lang_switch")
        if CTkToolTip is not None:
            CTkToolTip(self._lang_btn, text="")
        self._theme_btn = ctk.CTkButton(btn_frame, text="", width=40, height=36, command=self._toggle_theme)
        self._theme_btn.pack(side="right")
        if CTkToolTip is not None:
            CTkToolTip(self._theme_btn, text="")

        src_row = ctk.CTkFrame(outer, fg_color="transparent")
        src_row.pack(fill="x", pady=(0, 10))
        src_lbl = ctk.CTkLabel(src_row, text="", font=ctk.CTkFont(size=14))
        src_lbl.pack(side="left", padx=(0, 10))
        self._reg_widget(src_lbl, "source_folder")
        self._input_entry = ctk.CTkEntry(src_row, placeholder_text="", font=ctk.CTkFont(size=13), height=34)
        self._input_entry.pack(side="left", fill="x", expand=True, padx=(0, 8))
        self._reg_widget(self._input_entry, "source_ph", is_placeholder=True)
        self._input_browse_btn = ctk.CTkButton(src_row, text="", width=90, height=34, command=self._browse_input)
        self._input_browse_btn.pack(side="right")
        self._reg_widget(self._input_browse_btn, "browse")

        list_frame = ctk.CTkFrame(outer, fg_color="transparent")
        list_frame.pack(fill="both", expand=True, pady=(0, 8))
        tree_container = ctk.CTkFrame(list_frame, fg_color="transparent")
        tree_container.pack(fill="both", expand=True)
        self._tree = ttk.Treeview(tree_container, columns=("name", "format", "size"), show="headings", height=8, selectmode="none")
        self._tree.heading("name", text="")
        self._tree.heading("format", text="")
        self._tree.heading("size", text="")
        self._tree.column("name", width=380, minwidth=200)
        self._tree.column("format", width=80, minwidth=60)
        self._tree.column("size", width=100, minwidth=80)
        tree_scroll = ctk.CTkScrollbar(tree_container, orientation="vertical", command=self._tree.yview)
        self._tree.configure(yscrollcommand=tree_scroll.set)
        self._tree.pack(side="left", fill="both", expand=True)
        tree_scroll.pack(side="right", fill="y")
        self._style_tree()
        self._count_label = ctk.CTkLabel(list_frame, text="", font=ctk.CTkFont(size=11), text_color="gray60")
        self._count_label.pack(anchor="e", pady=(4, 0))

        out_row = ctk.CTkFrame(outer, fg_color="transparent")
        out_row.pack(fill="x", pady=(0, 10))
        out_lbl = ctk.CTkLabel(out_row, text="", font=ctk.CTkFont(size=14))
        out_lbl.pack(side="left", padx=(0, 10))
        self._reg_widget(out_lbl, "output_folder")
        self._output_entry = ctk.CTkEntry(out_row, placeholder_text="", font=ctk.CTkFont(size=13), height=34)
        self._output_entry.pack(side="left", fill="x", expand=True, padx=(0, 8))
        self._reg_widget(self._output_entry, "output_ph", is_placeholder=True)
        self._output_browse_btn = ctk.CTkButton(out_row, text="", width=90, height=34, command=self._browse_output)
        self._output_browse_btn.pack(side="right")
        self._reg_widget(self._output_browse_btn, "browse")

        fmt_row = ctk.CTkFrame(outer, fg_color="transparent")
        fmt_row.pack(fill="x", pady=(0, 10))
        fmt_lbl = ctk.CTkLabel(fmt_row, text="", font=ctk.CTkFont(size=14))
        fmt_lbl.pack(side="left", padx=(0, 10))
        self._reg_widget(fmt_lbl, "output_fmt")
        self._fmt_combo = ctk.CTkComboBox(fmt_row, values=OUTPUT_FORMATS, width=120, height=34,
                                           font=ctk.CTkFont(size=13), command=self._on_fmt_change,
                                           state="readonly")
        self._fmt_combo.set("FLAC")
        self._fmt_combo.pack(side="left")

        prog_frame = ctk.CTkFrame(outer, fg_color="transparent")
        prog_frame.pack(fill="x", pady=(0, 10))
        self._progress = ctk.CTkProgressBar(prog_frame, height=6)
        self._progress.pack(fill="x")
        self._progress.set(0)
        self._progress_label = ctk.CTkLabel(prog_frame, text="", font=ctk.CTkFont(size=11), text_color="gray60")
        self._progress_label.pack(anchor="w", pady=(3, 0))

        self._convert_btn = ctk.CTkButton(outer, text="", height=44, font=ctk.CTkFont(size=15, weight="bold"), command=self._start_conversion)
        self._convert_btn.pack(fill="x")
        self._reg_widget(self._convert_btn, "start")

    def _reg_widget(self, widget, key: str, is_placeholder: bool = False):
        self._widget_texts[widget] = (key, is_placeholder)

    def _apply_language(self, animate: bool = False):
        if animate:
            self._start_reveal_animation()
        else:
            self._set_all_texts()
        if CTkToolTip is not None:
            CTkToolTip(self._theme_btn, text=self._t("theme_tip"))
        self._tree.heading("name", text=self._t("file_name"))
        self._tree.heading("format", text=self._t("format"))
        self._tree.heading("size", text=self._t("size"))

    def _set_all_texts(self):
        for widget, (key, is_ph) in self._widget_texts.items():
            text = self._t(key)
            if is_ph:
                widget.configure(placeholder_text=text)
            else:
                widget.configure(text=text)

    def _start_reveal_animation(self):
        targets = {}
        for widget, (key, is_ph) in self._widget_texts.items():
            targets[widget] = (self._t(key), is_ph)
        if not targets:
            return
        total_frames = 10
        interval = 22
        def step(frame: int):
            progress = (frame + 1) / total_frames
            for widget, (target, is_ph) in targets.items():
                n = max(1, int(len(target) * progress + 0.5))
                shown = target[:n]
                if is_ph:
                    widget.configure(placeholder_text=shown)
                else:
                    widget.configure(text=shown)
            if frame + 1 < total_frames:
                self.after(interval, lambda: step(frame + 1))
            else:
                for widget, (target, is_ph) in targets.items():
                    if is_ph:
                        widget.configure(placeholder_text=target)
                    else:
                        widget.configure(text=target)
        step(0)

    def _toggle_theme(self):
        if self._theme_animating:
            return
        self._theme_animating = True
        new_mode = "dark" if ctk.get_appearance_mode() == "Light" else "light"
        def switch():
            ctk.set_appearance_mode(new_mode)
            self._style_tree()
            self._update_theme_icon()
            self._fade_in(3)
        self._fade_out(4, switch)

    def _fade_out(self, step: int, on_done):
        alpha = 0.3 + step * 0.175
        self.attributes("-alpha", alpha)
        if step > 0:
            self.after(20, lambda: self._fade_out(step - 1, on_done))
        else:
            on_done()

    def _fade_in(self, step: int):
        alpha = 0.3 + step * 0.175
        self.attributes("-alpha", alpha)
        if step < 4:
            self.after(20, lambda: self._fade_in(step + 1))
        else:
            self.attributes("-alpha", 1.0)
            self._theme_animating = False

    def _update_theme_icon(self):
        self._theme_btn.configure(text="\u263e" if ctk.get_appearance_mode() == "Light" else "\u2600")

    def _toggle_language(self):
        self._lang = "en" if self._lang == "zh" else "zh"
        self._apply_language(animate=True)

    def _on_fmt_change(self, value):
        self._out_fmt = value

    def _browse_input(self):
        path = filedialog.askdirectory(title=self._t("source_folder"))
        if not path:
            return
        self._input_dir = path
        self._input_entry.delete(0, "end")
        self._input_entry.insert(0, path)
        self._scan_files()

    def _browse_output(self):
        path = filedialog.askdirectory(title=self._t("output_folder"))
        if not path:
            return
        self._output_dir = path
        self._output_entry.delete(0, "end")
        self._output_entry.insert(0, path)

    def _scan_files(self):
        self._audio_files.clear()
        self._tree.delete(*self._tree.get_children())
        if not self._input_dir:
            return
        try:
            files = []
            for root, dirs, filenames in os.walk(self._input_dir):
                dirs[:] = [d for d in dirs if not d.startswith(".")]
                for name in filenames:
                    ext = os.path.splitext(name)[1].lower()
                    if ext in SUPPORTED_EXT:
                        full_path = os.path.join(root, name)
                        rel = os.path.relpath(full_path, self._input_dir)
                        files.append({"path": full_path, "name": rel.replace('\\', '/'), "format": ext[1:].upper(), "size_bytes": os.path.getsize(full_path)})
            files.sort(key=lambda f: f["name"].lower())
            for f in files:
                f["size_display"] = self._fmt_size(f["size_bytes"])
                self._audio_files.append(f)
                self._tree.insert("", "end", values=(f["name"], f["format"], f["size_display"]))
        except Exception as e:
            messagebox.showinfo(self._t("info"), self._t("scan_error").format(e))
        self._count_label.configure(text=self._t("files_count").format(len(self._audio_files)))

    def _start_conversion(self):
        if not self._audio_files:
            messagebox.showinfo(self._t("info"), self._t("no_files"))
            return
        if not self._output_dir:
            messagebox.showinfo(self._t("info"), self._t("no_output"))
            return
        self._stop_flag = False
        self._convert_btn.configure(state="disabled", text=self._t("converting"))
        self._progress.set(0)
        self._progress_label.configure(text=self._t("preparing"))
        threading.Thread(target=self._convert_all, daemon=True).start()

    def _convert_all(self):
        total = len(self._audio_files)
        max_workers = min((os.cpu_count() or 4), 8)
        out_fmt = self._out_fmt.lower()
        failed = []
        completed = 0
        last_update = 0.0
        _update = self._update_ui

        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures = {executor.submit(self._convert_one, info, out_fmt): info for info in self._audio_files}
            pending = set(futures.keys())
            while pending and not self._stop_flag:
                done, pending = wait(pending, timeout=0.05)
                for future in done:
                    info = futures[future]
                    name = info["name"]
                    try:
                        ok, reason = future.result()
                    except Exception as e:
                        ok, reason = False, _friendly_error(e)
                    if not ok:
                        failed.append(f"{name}({reason})" if reason else name)
                    completed += 1
                now = time.monotonic()
                if (now - last_update > 0.12 or completed == total) and completed > 0:
                    _update(progress=completed / total, label=f"{self._t('progress_fmt').format(completed, total)}")
                    last_update = now
                time.sleep(0.02)
            if self._stop_flag:
                for f in pending:
                    f.cancel()

        success = total - len(failed)
        _update(progress=1.0, label=self._t("ready"))
        detail = ""
        if failed:
            detail = "\n".join(failed[:15])
        msg = self._t("done_fmt").format(success, len(failed), detail)
        self.after(0, lambda: messagebox.showinfo(self._t("done_title"), msg))
        self.after(0, lambda: self._convert_btn.configure(state="normal", text=self._t("start")))

    def _convert_one(self, info: dict, out_fmt: str):
        ext_map = {"flac": ".flac", "wav": ".wav"}
        out_ext = ext_map.get(out_fmt, ".flac")
        output_path = os.path.join(self._output_dir, os.path.splitext(info["name"])[0] + out_ext)
        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        fmt = info["format"]

        if fmt == "FLAC":
            if out_fmt == "flac":
                try:
                    shutil.copy2(info["path"], output_path)
                    _copy_metadata(info["path"], output_path)
                    return True, ""
                except Exception as e:
                    return False, _friendly_error(e)
            source_path = info["path"]
        elif fmt == "NCM":
            try:
                temp_path, ncm_fmt = decrypt_ncm(info["path"], self._output_dir)
            except RuntimeError as e:
                return False, _friendly_error(e)
            if ncm_fmt == out_fmt:
                try:
                    if os.path.exists(output_path):
                        os.remove(output_path)
                    shutil.move(temp_path, output_path)
                    _copy_metadata(temp_path, output_path)
                    return True, ""
                except Exception as e:
                    _remove(temp_path)
                    return False, _friendly_error(e)
            source_path = temp_path
        else:
            source_path = info["path"]

        try:
            decoded = miniaudio.decode_file(source_path)
        except Exception as e:
            _remove(source_path if fmt == "NCM" else None)
            return False, _friendly_error(e)

        try:
            samples = np.array(decoded.samples, dtype=np.int16)
            if decoded.nchannels > 1:
                samples = samples.reshape(-1, decoded.nchannels)
            sf.write(output_path, samples, decoded.sample_rate, format=out_fmt.upper(), subtype="PCM_16")
            if os.path.getsize(output_path) < 44:
                _remove(output_path)
                return False, "输出文件过短"
            _copy_metadata(source_path, output_path)
            return True, ""
        except Exception as e:
            _remove(output_path)
            return False, _friendly_error(e)
        finally:
            if fmt == "NCM":
                _remove(source_path)

    def _update_ui(self, progress=0.0, label="", enable_btn=False):
        self.after(0, lambda: self._progress.set(progress))
        if label:
            self.after(0, lambda: self._progress_label.configure(text=label))
        if enable_btn:
            self.after(0, lambda: self._convert_btn.configure(state="normal", text=self._t("start")))

    def _style_tree(self):
        dark = ctk.get_appearance_mode() == "Dark"
        bg = "#2B2B2B" if dark else "#FFFFFF"
        fg = "#E0E0E0" if dark else "#1A1A1A"
        hdr_bg = "#3C3C3C" if dark else "#F3F3F3"
        hdr_fg = "#E0E0E0" if dark else "#333333"
        sel_bg = "#2E5A2E" if dark else "#E8F5E9"
        sel_fg = "#FFFFFF" if dark else "#1A1A1A"
        style = ttk.Style()
        style.theme_use("default")
        style.configure("Treeview", background=bg, foreground=fg, fieldbackground=bg, rowheight=28, borderwidth=1, relief="flat")
        style.configure("Treeview.Heading", background=hdr_bg, foreground=hdr_fg, font=("Segoe UI", 11, "bold"), relief="flat")
        style.map("Treeview", background=[("selected", sel_bg)], foreground=[("selected", sel_fg)])


if __name__ == "__main__":
    app = AudioConverterApp()
    app.mainloop()
