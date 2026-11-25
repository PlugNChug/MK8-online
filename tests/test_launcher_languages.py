"""Language switching against the real launcher; installer UI uses isolated API fixtures."""
import json
import re
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "apps/launcher/web"


class LanguageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pw = sync_playwright().start()
        cls.browser = cls.pw.chromium.launch(headless=True)

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.pw.stop()

    def start_launcher(self, folder):
        state_file = folder / "launcher.json"
        if state_file.exists():
            state_file.unlink()
        process = subprocess.Popen(
            [os.environ.get("MK8_LAUNCHER_EXE", str(ROOT / "target/release/Launcher.exe")),
             "--headless", "--data-dir", str(folder)],
            env={**os.environ, "MK8_NATIVE_TOOLS": str(folder / "native-tools")},
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        self.addCleanup(lambda: process.poll() is None and process.terminate())
        deadline = time.monotonic() + 15
        while not state_file.exists():
            if process.poll() is not None or time.monotonic() > deadline:
                self.fail("Launcher did not start")
            time.sleep(0.05)
        return process, json.loads(state_file.read_text())["url"]

    def test_language_survives_new_server_and_preserves_user_data(self):
        with tempfile.TemporaryDirectory(prefix="mk8-language-ui-") as temporary:
            folder = Path(temporary)
            process, url = self.start_launcher(folder)
            page = self.browser.new_page(locale="en-US", viewport={"width": 1365, "height": 1024})
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            try:
                with page.expect_request(lambda req: req.url.endswith("/api/status")) as initial_status:
                    page.goto(url)
                self.assertEqual(initial_status.value.headers["x-mk8-language"], "en")
                expect(page.get_by_role("heading", name="Installation", exact=True)).to_be_visible()
                page.get_by_role("button", name="Settings", exact=True).click()
                page.get_by_role("tab", name="Controls", exact=True).click()
                vibration = page.get_by_role("checkbox", name="Controller vibration")
                expect(vibration).to_be_checked()
                vibration.uncheck()
                page.get_by_role("button", name="Save settings", exact=True).click()
                expect(page.locator("#settings-save")).to_be_disabled()
                expect(vibration).to_be_enabled()
                self.assertIn("controller_vibration = 0", (folder / "user-no-mii/mk8d.ini").read_text())
                token = page.locator('meta[name="mk8-token"]').get_attribute("content")
                saved = page.request.post(url + "/api/username", headers={"X-MK8-Token": token}, data=json.dumps({"name": "Instalar"}))
                self.assertTrue(saved.json()["ok"])
                page.get_by_role("button", name="Multiplayer", exact=True).click()
                expect(page.locator("#mp-mii-text")).to_have_text("Create your Mii to open or join rooms.")
                page.locator("#friend-filter").fill("literal-private-value")
                page.get_by_label("Language", exact=True).select_option("pt-BR")
                expect(page.locator("html")).to_have_attribute("lang", "pt-BR")
                expect(page.locator("#mp-mii-text")).to_have_text("Crie seu Mii para abrir ou entrar em salas.")
                expect(page.locator("#friend-filter")).to_have_value("literal-private-value")
                with page.expect_request(lambda req: req.url.endswith("/api/status") and
                                          req.headers.get("x-mk8-language") == "es"):
                    page.get_by_label("Idioma", exact=True).select_option("es")
                expect(page.locator("html")).to_have_attribute("lang", "es")
                expect(page.locator("#mp-mii-text")).to_have_text("Crea tu Mii para abrir salas o unirte a ellas.")
                expect(page.get_by_role("button", name="Crear sala", exact=True)).to_be_visible()
                expect(page.locator("#friend-filter")).to_have_value("literal-private-value")
                page.get_by_label("Idioma", exact=True).select_option("en")
                expect(page.get_by_role("button", name="Create room", exact=True)).to_be_visible()
                expect(page.locator('#steam-host-manual')).to_have_text('Create manual room')
                expect(page.locator('#steam-navigation option[value="manual"]')).to_have_text('Navigate manually in game')
                page.get_by_role("button", name="Install", exact=True).click()
                expect(page.locator('#prepare-dlc-package')).to_have_attribute('placeholder', 'Leave empty to build without importing DLC')
                expect(page.locator("#flow-step-2")).to_be_disabled()
                expect(page.locator("#flow-guidance")).to_have_text("Install the tools to unlock game file selection.")
                response = page.request.post(url + "/api/language", headers={"X-MK8-Token": token}, data=json.dumps({"language": "fr"}))
                self.assertEqual(response.status, 400)
                self.assertEqual(json.loads((folder / "language.json").read_text()), "en")
                self.assertEqual(json.loads((folder / "username.json").read_text()), "Instalar")
                process.terminate()
                process.wait(timeout=10)
                process, url = self.start_launcher(folder)
                page.goto(url)
                expect(page.locator("html")).to_have_attribute("lang", "en")
                expect(page.get_by_role("heading", name="Installation", exact=True)).to_be_visible()
                page.get_by_role("button", name="Settings", exact=True).click()
                page.get_by_role("tab", name="Controls", exact=True).click()
                expect(vibration).to_be_enabled()
                expect(vibration).not_to_be_checked()
                page.get_by_role("button", name="Multiplayer", exact=True).click()
                expect(page.get_by_role("heading", name="Your room", exact=True)).to_be_visible()
                page.get_by_label("Language", exact=True).select_option("pt-BR")
                page.reload()
                expect(page.get_by_role("heading", name="Instalação", exact=True)).to_be_visible()
                page.get_by_label("Idioma", exact=True).select_option("es")
                expect(page.locator("html")).to_have_attribute("lang", "es")
                page.reload()
                expect(page.get_by_role("heading", name="Instalación", exact=True)).to_be_visible()
                self.assertEqual(json.loads((folder / "language.json").read_text()), "es")
                page.get_by_label("Idioma", exact=True).select_option("en")
                expect(page.locator("html")).to_have_attribute("lang", "en")
                token = page.locator('meta[name="mk8-token"]').get_attribute("content")
                status = page.request.get(url + "/api/status", headers={"X-MK8-Token": token}).json()["data"]
                status["build"].update(state="running", phase="Compilando para o seu PC", percent=42, started_ms=int(time.time() * 1000) - 60000)
                page.route("**/api/status", lambda route: route.fulfill(json={"ok": True, "data": status}))
                expect(page.locator("#build-phase")).to_have_text("Compiling for your PC")
                expect(page.locator("#build-summary")).to_have_text("Compiling for your PC · 42%")
                page.get_by_label("Language", exact=True).select_option("pt-BR")
                expect(page.locator("#build-phase")).to_have_text("Compilando para o seu PC")
                page.get_by_label("Idioma", exact=True).select_option("es")
                expect(page.locator("#build-phase")).to_have_text("Compilando para tu PC")
                expect(page.locator("#build-summary")).to_have_text("Compilando para tu PC · 42%")
                page.get_by_label("Idioma", exact=True).select_option("en")
                expect(page.locator("#build-phase")).to_have_text("Compiling for your PC")
                page.unroute("**/api/status")
                expect(page.locator("#build-live")).to_be_hidden()
                page.set_viewport_size({"width": 390, "height": 844})
                self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
                screenshots = ROOT / "local/screenshots"
                screenshots.mkdir(parents=True, exist_ok=True)
                page.screenshot(path=str(screenshots / "launcher-english-mobile.png"), full_page=True)
                page.set_viewport_size({"width": 1365, "height": 1024})
                page.screenshot(path=str(screenshots / "launcher-english.png"), full_page=True)
                self.assertEqual(errors, [])
            finally:
                page.close()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)

    def test_rebuild_control_uses_existing_preparation(self):
        """Recompilação visível, bilíngue e bloqueada durante operações, sem arquivos comerciais."""
        page = self.browser.new_page(locale="en-US", viewport={"width": 1365, "height": 1024})
        self.addCleanup(page.close)
        errors, requests = [], []
        page.on("pageerror", lambda error: errors.append(str(error)))
        status = {
            "version": "0.1.8", "room_menu": 0, "windowed": False, "last_report": None,
            "data_dir": "C:/fixture", "native_dialogs": False, "tools_ready": True,
            "setup": {"state": "idle"}, "files": {"folder": True, "ready": True},
            "checks": [], "candidate": True, "candidate_stale": False, "installed": False,
            "game_pid": None, "game_exit": None, "updates": {"configured": False},
            "session": {"hosting": False, "slot": None, "connected": False, "internet_available": False},
            "build": {"state": "candidate", "detail": "", "percent": 100,
                      "started_ms": None, "duration_s": 20},
        }
        config = {"exefs": "C:/fixture/exefs", "base_package": "", "update_package": "", "prod_keys": ""}
        language = "en"

        def route(request):
            nonlocal language
            path = request.request.url.split("mk8.test", 1)[1]
            if path.startswith("/api/"):
                body = request.request.post_data_json if request.request.method == "POST" else None
                requests.append((path, body))
                data = {}
                if path == "/api/status":
                    data = status
                elif path == "/api/config":
                    data = config
                elif path == "/api/game-folder":
                    data = {"config": config}
                elif path == "/api/username":
                    data = "Teste"
                elif path == "/api/language":
                    language = body["language"]
                request.fulfill(json={"ok": True, "data": data})
            else:
                file = WEB / ("index.html" if path == "/" else path.split("?", 1)[0][1:])
                text = {".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".cjs": "text/javascript",
                        ".json": "application/json", ".css": "text/css"}
                if file.suffix in text:
                    body = file.read_text(encoding="utf-8").replace("__TOKEN__", "test").replace("__LANGUAGE__", language)
                    # Same rewrite as apps/launcher/src/server.rs for the vendored FFL.js modules.
                    body = body.replace("from 'three'", "from '/vendor/three/three.module.js'")
                    request.fulfill(body=body, content_type=text[file.suffix] + "; charset=utf-8")
                else:  # The Mii editor's FFL.js also loads binary assets such as its .wasm module.
                    binary = {".wasm": "application/wasm", ".png": "image/png", ".svg": "image/svg+xml"}[file.suffix]
                    request.fulfill(body=file.read_bytes(), content_type=binary)

        page.route("http://mk8.test/**", route)
        page.goto("http://mk8.test/")
        page.get_by_role("button", name="Install", exact=True).click()
        rebuild = page.locator("#build")
        expect(rebuild).to_have_text("Rebuild game")
        expect(rebuild).to_be_visible()
        expect(rebuild).to_be_enabled()
        expect(page.locator("#build-done")).to_be_visible()
        expect(page.locator("#go-play")).to_be_visible()
        screenshots = ROOT / "local/screenshots"
        screenshots.mkdir(parents=True, exist_ok=True)
        for width, height in ((1365, 1024), (390, 844)):
            page.set_viewport_size({"width": width, "height": height})
            banner = page.locator("#build-done").bounding_box()
            for button in (rebuild, page.locator("#go-play")):
                expect(button).to_be_visible()
                box = button.bounding_box()
                self.assertGreaterEqual(box["y"], banner["y"] + banner["height"])
                self.assertGreaterEqual(box["x"], 0)
                self.assertLessEqual(box["x"] + box["width"], width)
            self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
            page.screenshot(path=str(screenshots / f"launcher-rebuild-{width}.png"), full_page=True, animations="disabled")
        page.set_viewport_size({"width": 1365, "height": 1024})
        page.get_by_label("Language", exact=True).select_option("pt-BR")
        expect(rebuild).to_have_text("Recompilar jogo")
        page.get_by_label("Idioma", exact=True).select_option("en")
        rebuild.click()
        expect(page.locator("#build-done")).to_be_visible()
        page.wait_for_function("!busy")
        self.assertEqual([body for path, body in requests if path == "/api/prepare"], [{"dlc_package": ""}])
        self.assertEqual([body for path, body in requests if path == "/api/game-folder"], [{"path": "C:/fixture"}])

        # Ao reabrir, o manifesto existe e o supervisor começa no estado idle.
        status["build"].update(state="idle")
        page.reload()
        page.get_by_role("button", name="Install", exact=True).click()
        expect(rebuild).to_have_text("Rebuild game")
        expect(rebuild).to_be_visible()
        for field, value in (("game_pid", 123), ("setup", {"state": "running", "steps": [], "total": 0})):
            previous = status[field]
            status[field] = value
            page.evaluate("refresh()")
            expect(rebuild).to_be_disabled()
            status[field] = previous
        status["build"].update(state="running")
        page.evaluate("refresh()")
        expect(rebuild).to_be_disabled()
        expect(rebuild).to_be_hidden()
        for state in ("failed", "cancelled"):
            status["build"].update(state=state)
            page.evaluate("refresh()")
            expect(rebuild).to_have_text("Try again")
            expect(rebuild).to_be_visible()
            expect(rebuild).to_be_enabled()
        status["build"].update(state="idle")
        status["candidate_stale"] = True
        page.evaluate("refresh()")
        expect(rebuild).to_have_text("Update build")
        status.update(candidate=False, candidate_stale=False)
        page.evaluate("refresh()")
        expect(rebuild).to_have_text("Start build")
        self.assertEqual(errors, [])

    def test_uninstaller_uses_saved_language_and_requires_full_removal_confirmation(self):
        page = self.browser.new_page()
        self.addCleanup(page.close)
        submissions = []

        def route(request):
            path = request.request.url.removeprefix("http://mk8.test/")
            if path == "api/state":
                request.fulfill(json={"ok": True, "data": {"version": "test", "data": "D:/Games/MK8"}})
            elif path == "api/uninstall":
                submissions.append(request.request.post_data_json)
                request.fulfill(json={"ok": True, "data": {}})
            elif not path:
                html = (WEB / "uninstall.html").read_text(encoding="utf-8")
                request.fulfill(body=html.replace("__LANGUAGE__", "en"), content_type="text/html; charset=utf-8")
            else:
                request.fulfill(path=WEB / path)

        page.route("http://mk8.test/**", route)
        for purge in (False, True):
            page.goto("http://mk8.test/")
            expect(page.locator("html")).to_have_attribute("lang", "en")
            expect(page.locator("[value=keep]")).to_be_checked()
            if purge:
                page.locator("[value=all]").check()
                expect(page.locator("#remove")).to_be_disabled()
                page.locator("#confirm").check()
            page.locator("#remove").click()
            expect(page.locator("#done")).to_be_visible()
            self.assertEqual(submissions[-1], {"purge": purge, "confirmed": purge})

    def test_installer_follows_spanish_locale_and_defaults_to_english(self):
        for locale, language, heading in (("es-MX", "es", "Bienvenido a MK8-online"),
                                          ("de-DE", "en", "Welcome to MK8-online")):
            with self.subTest(locale=locale):
                self.installer_language(locale, language, heading)

    def installer_language(self, locale, language, heading):
        page = self.browser.new_page(locale=locale, viewport={"width": 880, "height": 620})

        def route(request):
            path = request.request.url.split("mk8.test", 1)[1]
            if path.startswith("/api/"):
                request.fulfill(json={"ok": True, "data": {"version": "0.2.0", "app": "C:\\Apps\\MK8-online", "data": "D:\\Games\\MK8-online", "desktop": True, "update": False, "needed_gib": 30}})
                return
            file = WEB / ("setup.html" if path == "/" else path[1:])
            mime = {".html": "text/html", ".js": "text/javascript", ".json": "application/json", ".css": "text/css"}[file.suffix]
            request.fulfill(body=file.read_text(encoding="utf-8").replace("__TOKEN__", "test").replace("__LANGUAGE__", ""), content_type=mime + "; charset=utf-8")

        try:
            page.route("http://mk8.test/**", route)
            page.goto("http://mk8.test/")
            expect(page.get_by_role("heading", name=heading)).to_be_visible()
            expect(page.locator("html")).to_have_attribute("lang", language)
            expect(page.locator("#language")).to_have_value(language)
        finally:
            page.close()

    def test_installer_refuses_data_folder_without_hard_links(self):
        page = self.browser.new_page(locale="en-US", viewport={"width": 880, "height": 620})

        def route(request):
            path = request.request.url.split("mk8.test", 1)[1]
            if path.startswith("/api/"):
                if path == "/api/state":
                    data = {"version": "0.4.0", "app": "C:\\Apps\\MK8-online", "data": "D:\\Games\\MK8-online", "desktop": True, "update": False, "needed_gib": 30}
                elif path == "/api/space":
                    data = {"free_gib": 400.0, "hard_disk": False, "file_system": "exFAT", "hard_links": False}
                else:
                    data = {}
                request.fulfill(json={"ok": True, "data": data})
                return
            file = WEB / ("setup.html" if path == "/" else path[1:])
            mime = {".html": "text/html", ".js": "text/javascript", ".json": "application/json", ".css": "text/css"}[file.suffix]
            request.fulfill(body=file.read_text(encoding="utf-8").replace("__TOKEN__", "test").replace("__LANGUAGE__", ""), content_type=mime + "; charset=utf-8")

        try:
            page.route("http://mk8.test/**", route)
            page.goto("http://mk8.test/")
            page.get_by_role("button", name="Get started").click()
            page.locator("#data").fill("G:\\Jeux\\MK8-online")
            expect(page.locator("#space")).to_have_text(
                "The drive of the data folder uses exFAT, which does not support hard links. Game preparation needs an NTFS drive: choose another folder or convert the drive to NTFS.")
            expect(page.locator("#space")).to_have_class(re.compile(r"\bbad\b"))
            page.get_by_label("Language", exact=True).select_option("es")
            expect(page.locator("#space")).to_contain_text("unidad exFAT, que no admite enlaces físicos")
        finally:
            page.close()

    def test_installer_switches_language_and_keeps_paths(self):
        page = self.browser.new_page(locale="pt-BR", viewport={"width": 880, "height": 620})
        errors = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        requests = []

        def route(request):
            path = request.request.url.split("mk8.test", 1)[1]
            if path.startswith("/api/"):
                body = request.request.post_data_json if request.request.method == "POST" else {}
                requests.append((path, body))
                if path == "/api/state":
                    data = {"version": "0.1.5", "app": "C:\\Apps\\MK8-online", "data": "D:\\Games\\MK8-online", "desktop": True, "update": False, "needed_gib": 30}
                elif path == "/api/space":
                    data = {"free_gib": 21.5, "hard_disk": True}
                elif path == "/api/install":
                    request.fulfill(json={"ok": False, "error": "A instalação já está em andamento."}, status=400)
                    return
                else:
                    data = {}
                request.fulfill(json={"ok": True, "data": data})
            else:
                file = WEB / ("setup.html" if path == "/" else path[1:])
                mime = {".html": "text/html", ".js": "text/javascript", ".json": "application/json", ".css": "text/css"}[file.suffix]
                request.fulfill(body=file.read_text(encoding="utf-8").replace("__TOKEN__", "test").replace("__LANGUAGE__", ""), content_type=mime + "; charset=utf-8")

        try:
            page.route("http://mk8.test/**", route)
            page.goto("http://mk8.test/")
            expect(page.get_by_role("heading", name="Bem-vindo ao MK8-online")).to_be_visible()
            page.get_by_label("Idioma", exact=True).select_option("es")
            expect(page.get_by_role("heading", name="Bienvenido a MK8-online")).to_be_visible()
            page.get_by_label("Idioma", exact=True).select_option("en")
            expect(page.get_by_role("heading", name="Welcome to MK8-online")).to_be_visible()
            page.get_by_role("button", name="Get started").click()
            page.locator("#data").fill("E:\\Instalar\\MK8-online")
            expect(page.locator("#space")).to_contain_text("Only 21.5 GB free")
            expect(page.locator("#space")).to_contain_text("hard drive")
            page.get_by_role("button", name="Install", exact=True).click()
            expect(page.locator("#paths-error")).to_have_text("Installation is already in progress.")
            self.assertEqual(next(body["language"] for path, body in requests if path == "/api/install"), "en")
            page.get_by_label("Language", exact=True).select_option("pt-BR")
            expect(page.locator("#space")).to_contain_text("Só 21,5 GB")
            expect(page.locator("#paths-error")).to_have_text("A instalação já está em andamento.")
            expect(page.locator("#data")).to_have_value("E:\\Instalar\\MK8-online")
            page.get_by_label("Idioma", exact=True).select_option("es")
            expect(page.locator("#space")).to_contain_text("Solo hay 21,5 GB libres")
            expect(page.locator("#space")).to_contain_text("disco duro")
            expect(page.locator("#paths-error")).to_have_text("La instalación ya está en curso.")
            page.get_by_label("Idioma", exact=True).select_option("en")
            expect(page.locator("html")).to_have_attribute("lang", "en")
            self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
            screenshots = ROOT / "local/screenshots"
            screenshots.mkdir(parents=True, exist_ok=True)
            page.screenshot(path=str(screenshots / "installer-english.png"), full_page=True)
            self.assertEqual(errors, [])
        finally:
            page.close()


    def test_installer_finishes_once_and_can_retry_opening(self):
        for launch in (True, False):
            with self.subTest(launch=launch):
                page = self.browser.new_page(locale="en-US")
                page.clock.install()
                progress = []
                finishes = []

                def route(request):
                    path = request.request.url.removeprefix("http://mk8.test")
                    if path == "/api/progress":
                        progress.append(request)
                        return
                    if path == "/api/finish":
                        finishes.append(request.request.post_data_json["launch"])
                        if launch and len(finishes) == 1:
                            request.fulfill(status=400, json={"ok": False, "error": "Initialization failed"})
                            return
                    if path.startswith("/api/"):
                        request.fulfill(json={"ok": True, "data": {
                            "version": "0.3.0", "app": r"C:\Apps\MK8-online", "data": r"D:\Games\MK8-online",
                            "desktop": False, "update": False, "needed_gib": 30, "free_gib": 40,
                        }})
                        return
                    file = WEB / ("setup.html" if path == "/" else path[1:])
                    request.fulfill(path=file)

                try:
                    page.route("http://mk8.test/**", route)
                    page.goto("http://mk8.test/")
                    page.locator("#go-paths").click()
                    expect(page.locator("#data")).to_have_value(r"D:\Games\MK8-online")
                    page.locator("#launch").set_checked(launch)
                    page.locator("#install").click()
                    expect(page.locator("#progress")).to_be_visible()
                    with page.expect_request("**/api/progress"):
                        page.clock.run_for(250)
                    page.clock.run_for(1000)
                    self.assertEqual(len(progress), 1)
                    progress[0].fulfill(json={"ok": True, "data": {"percent": 100, "state": "done"}})
                    expect(page.locator("#done")).to_be_visible()
                    if launch:
                        expect(page.get_by_role("alert")).to_have_text("Initialization failed")
                        self.assertEqual(finishes, [True])
                        with page.expect_response("**/api/finish"):
                            page.locator("#open").click()
                        self.assertEqual(finishes, [True, True])
                    else:
                        page.clock.run_for(1000)
                        self.assertEqual(finishes, [])
                        with page.expect_response("**/api/finish"):
                            page.locator("#close").click()
                        self.assertEqual(finishes, [False])
                finally:
                    page.close()


if __name__ == "__main__":
    unittest.main()
