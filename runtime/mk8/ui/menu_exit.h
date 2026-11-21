#pragma once
#include "menu_language.h"
#include "native_rows_host.h"
extern "C" bool mk8_settings_save_checked(void);

// Draw the error next to the same action so the player can retry in place.
static bool mk8_menu_exit_ready(bool pressed,bool &failed) {
    if (pressed) failed=!mk8_settings_save_checked();
    const char *message=mk8_menu_text(mk8_menu_language(),
        "Could not save settings. Check the settings folder and try again.",
        "Não foi possível salvar os ajustes. Verifique a pasta de configurações e tente novamente.",
        "No se pudieron guardar los ajustes. Comprueba la carpeta de configuración e inténtalo de nuevo.");
    if (mk8_collecting_rows() && (pressed || failed)) mk8_collect_exit_feedback(failed,message);
    else if (failed) ImGui::TextWrapped("%s",message);
    return pressed && !failed;
}
