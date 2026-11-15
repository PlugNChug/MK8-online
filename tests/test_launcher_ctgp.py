"""CTGP Deluxe card in the launcher (Settings), with the real launcher API and synthetic states."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]


class LauncherCtgpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="mk8-ctgp-ui-")
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

    def page(self, locale="pt-BR"):
        page = self.browser.new_page(locale=locale)
        self.addCleanup(page.close)
        errors = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        self.addCleanup(lambda: self.assertEqual(errors, []))
        page.goto(self.url)
        page.wait_for_load_state("networkidle")
        return page

    def token(self, page):
        return page.locator('meta[name="mk8-token"]').get_attribute("content")

    def test_unprepared_game_keeps_ctgp_off_and_the_api_refuses_it(self):
        page = self.page()
        page.locator('[data-tab="settings"]').click()
        expect(page.locator("#ctgp-card")).to_be_visible()
        expect(page.locator("#ctgp-import")).to_be_disabled()
        expect(page.locator("#ctgp-enabled")).to_be_disabled()
        expect(page.locator("#ctgp-hint")).to_have_text("Prepare o jogo antes de importar o CTGP.")
        headers = {"X-MK8-Token": self.token(page)}
        for path, body in (("ctgp/toggle", {"enabled": True}), ("ctgp/import", {"source": str(self.folder)})):
            response = page.request.post(f"{self.url}/api/{path}", headers=headers, data=body).json()
            self.assertFalse(response["ok"])
        self.assertFalse((self.folder / "ctgp.json").exists())
        self.assertFalse((self.folder / "ctgp").exists())

    def test_import_and_switch_from_the_card(self):
        page = self.page()
        state = page.request.get(self.url + "/api/status", headers={"X-MK8-Token": self.token(page)}).json()["data"]
        state.update(candidate=True, candidate_stale=False)
        state["ctgp"] = {"state": "missing", "installed": False, "enabled": False, "detail": "", "courses": 0,
                         "base_courses": 0, "names_verified": False}
        state["ctgp_import"] = {"state": "idle", "detail": ""}
        page.route("**/api/status", lambda route: route.fulfill(json={"ok": True, "data": state}))
        calls = []

        def importing(route):
            calls.append(("import", route.request.post_data_json))
            state["ctgp_import"]["state"] = "running"
            route.fulfill(json={"ok": True, "data": {"message": "Importação do CTGP iniciada."}})

        def toggling(route):
            enabled = route.request.post_data_json["enabled"]
            calls.append(("toggle", enabled))
            state["ctgp"]["enabled"] = enabled
            route.fulfill(json={"ok": True, "data": {"message": "Preferência do CTGP salva."}})
        page.route("**/api/ctgp/import", importing)
        page.route("**/api/ctgp/toggle", toggling)
        page.route("**/api/ctgp/log", lambda route: route.fulfill(json={"ok": True, "data": {"text": "Montando 1088 arquivos (hardlinks)."}}))
        page.reload()
        page.locator('[data-tab="settings"]').click()
        expect(page.locator("#ctgp-state")).to_have_text("Não instalado")
        expect(page.locator("#ctgp-enabled")).to_be_disabled()
        expect(page.locator("#ctgp-import")).to_be_enabled()
        page.locator("#ctgp-source").fill("D:/Jogos/CTGPDX")
        page.locator("#ctgp-import").click()
        expect(page.locator("#ctgp-state")).to_have_text("Importando…")
        expect(page.locator("#ctgp-import")).to_be_disabled()
        self.assertEqual(calls, [("import", {"source": "D:/Jogos/CTGPDX", "reinstall": False})])
        state["ctgp"].update(state="ready", installed=True, enabled=True, courses=64, base_courses=48,
                             names_verified=True)
        state["ctgp_import"]["state"] = "done"
        expect(page.locator("#ctgp-state")).to_have_text("Ativado", timeout=5000)
        expect(page.locator("#ctgp-enabled")).to_be_checked()
        expect(page.locator("#ctgp-hint")).to_have_text("64 pistas do CTGP no lugar das originais. Vale ao iniciar o jogo.")
        expect(page.locator("#ctgp-import")).to_have_text("Importar novamente…")
        page.locator("#ctgp-enabled").uncheck()
        expect(page.locator("#ctgp-state")).to_have_text("Desativado")
        expect(page.locator("#ctgp-hint")).to_have_text("Jogo original. Ative para jogar as 64 pistas do CTGP.")
        self.assertEqual(calls[-1], ("toggle", False))
        state["game_pid"] = 1234
        expect(page.locator("#ctgp-enabled")).to_be_disabled(timeout=5000)
        expect(page.locator("#ctgp-hint")).to_have_text("Feche o jogo para alterar o CTGP.")
        state["game_pid"] = None
        for language, text in (("en", "Original game. Enable it to play the 64 CTGP courses."),
                               ("es", "Juego original. Actívalo para jugar las 64 pistas de CTGP.")):
            page.locator("#language").select_option(language)
            expect(page.locator("#ctgp-hint")).to_have_text(text, timeout=5000)
        page.set_viewport_size({"width": 390, "height": 844})
        self.assertTrue(page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
        screenshots = ROOT / "local/screenshots"
        screenshots.mkdir(parents=True, exist_ok=True)
        page.locator("#ctgp-card").screenshot(path=str(screenshots / "launcher-ctgp-card.png"))
        page.locator("#language").select_option("pt-BR")


if __name__ == "__main__":
    unittest.main()
