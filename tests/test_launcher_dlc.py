"""Controles de DLC no navegador, com API real e estados de conteúdo autoral."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]


class LauncherDlcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="mk8-dlc-ui-")
        cls.folder = Path(cls.temporary.name)
        cls.process = subprocess.Popen(
            [os.environ.get("MK8_LAUNCHER_EXE", str(ROOT / "target/release/Launcher.exe")),
             "--headless", "--data-dir", str(cls.folder)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 20
        while not (cls.folder / "launcher.json").exists():
            if cls.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Launcher não iniciou")
            time.sleep(.05)
        cls.url = json.loads((cls.folder / "launcher.json").read_text())["url"]
        cls.pw = sync_playwright().start()
        cls.browser = cls.pw.chromium.launch()

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.pw.stop()
        cls.process.terminate()
        cls.process.wait(timeout=15)
        cls.temporary.cleanup()

    def page(self):
        page = self.browser.new_page(locale="pt-BR")
        self.addCleanup(page.close)
        page.goto(self.url)
        page.wait_for_load_state("networkidle")
        return page

    def test_unprepared_import_is_disabled_and_api_refuses_enable(self):
        page = self.page()
        expect(page.locator("#dlc-card")).to_be_visible()
        expect(page.locator("#dlc-import")).to_be_disabled()
        expect(page.locator("#dlc-enabled")).to_be_disabled()
        token = page.locator('meta[name="mk8-token"]').get_attribute("content")
        response = page.request.post(self.url + "/api/dlc/toggle",
                                     headers={"X-MK8-Token": token}, data={"enabled": True}).json()
        self.assertFalse(response["ok"])
        self.assertFalse((self.folder / "dlc.json").exists())

    def test_file_tiles_show_green_only_for_files_found_by_the_api(self):
        package = self.folder / "fixture.nsp"
        keys = self.folder / "fixture.keys"
        package.write_bytes(b"pacote autoral")
        keys.write_bytes(b"chaves autorais")
        page = self.page()
        page.locator("#dlc-package").fill(str(package))
        page.locator("#dlc-keys").fill(str(keys))
        expect(page.locator("#tile-dlc-package")).to_have_attribute("data-ok", "true")
        expect(page.locator("#tile-dlc-keys")).to_have_attribute("data-ok", "true")
        self.assertEqual(page.locator("#tile-dlc-package .ft-icon").evaluate(
            "el => getComputedStyle(el, '::after').content"), '"✓"')
        held = []
        def delay_old_path(route):
            if route.request.post_data_json["package"] == str(package):
                held.append((route, route.fetch()))
            else:
                route.continue_()
        page.route("**/api/dlc/files", delay_old_path)
        page.locator("#dlc-package").fill("")
        page.locator("#dlc-package").fill(str(package))
        page.wait_for_timeout(500)
        self.assertTrue(held)
        page.locator("#dlc-package").fill(str(self.folder / "missing.nsp"))
        expect(page.locator("#tile-dlc-package")).to_have_attribute("data-ok", "false")
        expect(page.locator("#tile-dlc-keys")).to_have_attribute("data-ok", "true")
        for route, response in held:
            route.fulfill(response=response)
        page.wait_for_timeout(300)
        expect(page.locator("#tile-dlc-package")).to_have_attribute("data-ok", "false")
        page.locator("#dlc-keys").fill(str(self.folder / "missing.keys"))
        expect(page.locator("#tile-dlc-keys")).to_have_attribute("data-ok", "false")

    def test_installed_dlc_can_be_reinstalled_from_a_selected_package(self):
        page = self.page()
        token = page.locator('meta[name="mk8-token"]').get_attribute("content")
        state = page.request.get(self.url + "/api/status", headers={"X-MK8-Token": token}).json()["data"]
        state.update(candidate=True, candidate_stale=False)
        state["dlc"] = {"state":"ready", "installed":True, "enabled":False, "detail":""}
        state["dlc_import"] = {"state":"idle", "detail":""}
        page.route("**/api/status", lambda route: route.fulfill(json={"ok":True,"data":state}))
        imports = []
        def run(route):
            imports.append(route.request.post_data_json)
            state["dlc_import"]["state"] = "running"
            route.fulfill(json={"ok":True,"data":{}})
        page.route("**/api/dlc/import", run)
        page.reload()
        page.locator('[data-tab="install"]').click()
        expect(page.locator("#dlc-files")).to_be_hidden()
        expect(page.locator("#dlc-change")).to_be_visible()
        page.locator("#dlc-change").click()
        page.locator("#dlc-package").fill("D:/Switch/Other DLC.nsp")
        page.locator("#dlc-keys").fill("D:/Switch/prod.keys")
        page.locator("#dlc-import").click()
        expect(page.locator("#dlc-import")).to_be_disabled()
        self.assertEqual(imports, [{"package":"D:/Switch/Other DLC.nsp","keys":"D:/Switch/prod.keys","reinstall":True}])
        state["dlc_import"]["state"] = "done"
        expect(page.locator("#dlc-files")).to_be_hidden(timeout=5000)
        expect(page.locator("#dlc-change")).to_be_enabled()
        page.set_viewport_size({"width":390, "height":844})
        self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
        page.locator('[data-tab="settings"]').click()
        expect(page.locator("#dlc-enabled")).not_to_be_checked()

    def test_controls_translate_toggle_report_errors_and_lock_during_operations(self):
        page = self.page()
        token = page.locator('meta[name="mk8-token"]').get_attribute("content")
        state = page.request.get(self.url + "/api/status", headers={"X-MK8-Token": token}).json()["data"]
        state.update(candidate=True, candidate_stale=False)
        state["dlc"] = {"state": "ready", "installed": True, "enabled": True, "detail": ""}
        state["dlc_import"] = {"state": "idle", "detail": "", "steps": [], "completed": 0, "total": 1}
        calls = []

        def status(route):
            route.fulfill(json={"ok": True, "data": state})

        def toggle(route):
            value = route.request.post_data_json
            calls.append(value)
            state["dlc"]["enabled"] = value["enabled"]
            route.fulfill(json={"ok": True, "data": {"message": "Preferência de DLC salva."}})

        page.route("**/api/status", status)
        page.route("**/api/dlc/toggle", toggle)
        page.reload()
        page.locator('[data-tab="settings"]').click()
        expect(page.locator("#dlc-enabled")).to_be_checked()
        page.locator("#dlc-enabled").uncheck()
        expect(page.locator("#dlc-state")).to_have_text("Desativado")
        self.assertEqual(calls, [{"enabled": False}])
        page.locator("#language").select_option("en")
        expect(page.locator("#dlc-card")).to_contain_text("Additional content")
        expect(page.locator("#dlc-state")).to_have_text("Disabled")
        screenshots = ROOT / "local/screenshots"
        screenshots.mkdir(parents=True, exist_ok=True)
        page.screenshot(path=str(screenshots / "dlc-launcher-desktop.png"), full_page=True)
        state["game_pid"] = 123
        expect(page.locator("#dlc-enabled")).to_be_disabled(timeout=5000)
        expect(page.locator('#settings input[data-key="volume"]')).to_be_disabled()
        expect(page.locator("#dlc-import")).to_be_disabled()
        state["game_pid"] = None
        state["dlc_import"]["state"] = "running"
        expect(page.locator("#play-solo")).to_be_disabled()
        expect(page.locator("#dlc-enabled")).to_be_disabled()
        state["dlc_import"]["state"] = "failed"
        state["dlc_import"]["detail"] = "DLC: pacote incompatível."
        expect(page.locator("#dlc-detail")).to_contain_text("DLC: pacote incompatível.", timeout=5000)
        state["dlc"]["state"] = "invalid"
        state["dlc"]["installed"] = False
        page.locator('[data-tab="install"]').click()
        expect(page.locator("#dlc-import")).to_have_text("Repair import…", timeout=5000)
        imports = []
        def import_package(route):
            imports.append(route.request.post_data_json)
            state["dlc_import"]["state"] = "running"
            route.fulfill(json={"ok": True, "data": {"message": "Importação do DLC iniciada."}})
        page.route("**/api/dlc/import", import_package)
        page.locator("#dlc-package").fill("D:/Switch/DLC.nsp")
        page.locator("#dlc-keys").fill("D:/Switch/prod.keys")
        page.locator("#dlc-import").click()
        expect(page.locator("#dlc-import")).to_be_disabled()
        self.assertEqual(imports, [{"package": "D:/Switch/DLC.nsp", "keys": "D:/Switch/prod.keys", "reinstall": False}])
        page.set_viewport_size({"width": 390, "height": 844})
        self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
        page.screenshot(path=str(screenshots / "dlc-launcher-mobile.png"), full_page=True)
        state["dlc_import"]["state"] = "idle"
        state["dlc"]["state"] = "unsupported"
        expect(page.locator("#dlc-import")).to_be_disabled()
        expect(page.locator("#dlc-hint")).to_have_text("Update the build to use DLC.", timeout=5000)
        expect(page.locator("#build-idle")).to_be_visible()
        expect(page.locator("#build")).to_have_text("Update build")
