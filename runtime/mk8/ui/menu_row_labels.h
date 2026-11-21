#pragma once
#include "menu_language.h"

typedef struct { int group; Mk8MenuLabel label,tip; } Mk8StockRowLabel;
static const Mk8MenuLabel mk8_progress_warning={
    "Unsaved progress will be lost.","O progresso não salvo será perdido.","Se perderá el progreso sin guardar."
};
static const Mk8StockRowLabel mk8_stock_labels[]={
    {MENU_QUIT, {"Restart the Game","Reiniciar o jogo","Reiniciar el juego"},mk8_progress_warning},
    {MENU_QUIT, {"Quit to Start Screen","Voltar à tela inicial","Volver a la pantalla inicial"},mk8_progress_warning},
    {MENU_QUIT, {"Quit to Desktop","Sair do jogo","Salir del juego"},mk8_progress_warning},
    {MENU_QUIT, {"Yes, restart the game","Sim, reiniciar","Sí, reiniciar"},mk8_progress_warning},
    {MENU_QUIT, {"Yes, quit to Start Screen","Sim, voltar ao início","Sí, volver al inicio"},mk8_progress_warning},
    {MENU_QUIT, {"Yes, quit to Desktop","Sim, sair do jogo","Sí, salir del juego"},mk8_progress_warning},
    {MENU_QUIT, {"Cancel","Cancelar","Cancelar"},{"Keep playing.","Continuar jogando.","Seguir jugando."}},
    {MENU_ACCOUNT, {"Online backend","Serviço online","Servicio en línea"},{"Applies after a restart.","Reinicie o jogo para aplicar.","Reinicia el juego para aplicar."}},
    {MENU_ACCOUNT, {"Restart the game now","Reiniciar o jogo agora","Reiniciar el juego ahora"},mk8_progress_warning},
    {MENU_ACCOUNT, {"Nextendo account","Conta Nextendo","Cuenta Nextendo"},{}},  // Keep service errors.
    {MENU_ACCOUNT, {"Cancel sign-in","Cancelar login","Cancelar inicio de sesión"},{"Stop waiting for browser sign-in.","Cancelar a espera pelo navegador.","Dejar de esperar al navegador."}},
    {MENU_ACCOUNT, {"Sign out","Sair da conta","Cerrar sesión"},{"Sign out of this device.","Sair da conta neste dispositivo.","Cerrar sesión en este dispositivo."}},
    {MENU_MODS, {"Show","Exibir","Mostrar"},{"Choose a mod list.","Escolha uma lista de mods.","Elige una lista de mods."}},
    {MENU_MODS, {"Search installed","Buscar instalados","Buscar instalados"},{"Filter by name, author or category.","Filtre por nome, autor ou categoria.","Filtra por nombre, autor o categoría."}},
    {MENU_MODS, {"Restart the game to apply","Reiniciar para aplicar","Reiniciar para aplicar"},mk8_progress_warning},
    {MENU_MODS, {"Load earlier","Carregar antes","Cargar antes"},{"Move one place earlier.","Mova uma posição para cima.","Sube una posición."}},
    {MENU_MODS, {"Load later","Carregar depois","Cargar después"},{"Move one place later.","Mova uma posição para baixo.","Baja una posición."}},
    {MENU_MODS, {"Remove","Remover","Eliminar"},{"Remove the mod from this device.","Remover o mod deste dispositivo.","Eliminar el mod de este dispositivo."}},
    {MENU_MODS, {"Yes, remove it","Sim, remover","Sí, eliminar"},{"Delete this mod from this device.","Excluir este mod deste dispositivo.","Borrar este mod de este dispositivo."}},
    {MENU_MODS, {"Cancel","Cancelar","Cancelar"},{"Keep the mod installed.","Manter o mod instalado.","Conservar el mod instalado."}},
};
