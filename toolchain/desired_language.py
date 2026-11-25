"""Expose selectable game languages and report the codes MK8 declares.

The 4.0.1 NACP declares Simplified and Traditional Chinese (application languages 14 and 13). The console reports those
through GetDesiredLanguage as zh-Hans / zh-Hant; zh-CN / zh-TW are the older system codes. The game compares the
desired code with the ones it builds itself, so a zh-CN reply fell back to English. The INI and settings_language()
keep zh-CN / zh-TW, which are saved values.

The 4.0.1 game also declares Spanish (Latin America) (USes), using es-419 as both its system and application code.
"""


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Game language boundary changed: ' + old[:80])
    return text.replace(old, new)


DESIRED = '''        snprintf(lang, sizeof lang, "%s", mk8_desired_language(settings_language()));'''
HELPER = '''// (toolchain/desired_language.py) The application languages behind the saved Chinese system codes.
static const char *mk8_desired_language(const char *system) {
    if (!strcmp(system, "zh-CN")) return "zh-Hans";
    if (!strcmp(system, "zh-TW")) return "zh-Hant";
    return system;
}

'''


def prepare(runtime):
    settings_header = runtime/'settings.h'
    header = settings_header.read_text(encoding='utf-8')
    header = replace(header, 'LANG_AUTO = 0, N_LANGUAGES = 12', 'LANG_AUTO = 0, N_LANGUAGES = 13')
    settings_header.write_text(header, encoding='utf-8')

    settings_source = runtime/'settings.c'
    settings = settings_source.read_text(encoding='utf-8')
    settings = replace(settings, '"zh-CN", "zh-TW"};', '"zh-CN", "zh-TW", "es-419"};')
    settings = replace(settings, '"English", "Spanish", "French"', '"English", "Spanish (Spain)", "French"')
    settings = replace(settings, '"Chinese (Traditional)"};', '"Chinese (Traditional)",\n'
                       '                                                   "Spanish (Latin America)"};')
    settings_source.write_text(settings, encoding='utf-8')

    am = runtime/'services/am.c'
    text = am.read_text(encoding='utf-8')
    reply = '        snprintf(lang, sizeof lang, "%s", settings_language());'
    command = '    case 21: {  // GetDesiredLanguage'
    if text.count(command) != 1:
        raise ValueError('GetDesiredLanguage command changed upstream')
    start = text.index(command)
    if text.find(reply, start) != text.index(reply):
        raise ValueError('GetDesiredLanguage reply moved upstream')
    text = replace(text, reply, DESIRED)
    # The helper goes ahead of the command table that uses it, at file scope.
    table = 'static void application_functions(Service *s, IpcReq *req, IpcResp *resp) {'
    text = replace(text, table, HELPER + table)
    am.write_text(text, encoding='utf-8')
