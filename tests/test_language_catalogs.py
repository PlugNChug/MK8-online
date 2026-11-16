"""Every translation catalog covers the same Portuguese source messages."""
import json
from pathlib import Path
import re
import unittest

WEB = Path(__file__).resolve().parents[1] / "apps/launcher/web"
PLACEHOLDER = re.compile(r"\{\d+\}")


def catalog(name):
    return json.loads((WEB / name).read_text(encoding="utf-8"))


class CatalogTests(unittest.TestCase):
    def test_spanish_matches_english_keys_and_placeholders(self):
        english, spanish = catalog("en.json"), catalog("es.json")
        self.assertEqual(set(spanish), set(english))
        for key, value in spanish.items():
            with self.subTest(key=key):
                self.assertTrue(value.strip())
                self.assertEqual(sorted(PLACEHOLDER.findall(value)), sorted(PLACEHOLDER.findall(english[key])))

    def test_pickers_offer_every_catalog(self):
        for page in ("index.html", "setup.html"):
            html = (WEB / page).read_text(encoding="utf-8")
            for language in ("es", "en", "pt-BR"):
                self.assertIn(f'<option value="{language}">', html, page)


if __name__ == "__main__":
    unittest.main()
