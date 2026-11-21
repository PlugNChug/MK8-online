#pragma once

// The NES font is wide. Move the value to its own line when label and controls do not fit.
struct Mk8NesRow {
    float label_wrap, value_wrap, value_width, height, label_top, value_top;
};

static Mk8NesRow mk8_nes_row(const char *label, const char *value, bool arrows, float width, float scale, float minimum) {
    const float fs=ImGui::GetFontSize(), px=fmaxf(2,floorf(fs*.15f)), pad=16*scale;
    const float inside=fmaxf(1,width-2*pad-8*px);
    const float arrow_space=arrows && *value ? 2*(5*px+.55f*fs) : 0;
    const float value_wrap=fmaxf(1,inside-arrow_space);
    const float value_width=fminf(ImGui::CalcTextSize(value).x,value_wrap);
    const bool separate=*value && ImGui::CalcTextSize(label).x+fs+value_width+arrow_space>inside;
    const float label_wrap=separate || !*value ? inside : fmaxf(1,inside-value_width-arrow_space-fs);
    const float lh=ImGui::CalcTextSize(label,nullptr,false,label_wrap).y;
    const float vh=ImGui::CalcTextSize(value,nullptr,false,value_wrap).y;
    const float gap=separate ? .35f*fs : 0;
    const float text_height=separate ? lh+gap+vh : fmaxf(lh,vh);
    const float height=fmaxf(minimum,text_height+fs);
    return {label_wrap,value_wrap,value_width,height,(height-text_height)*.5f,
            separate ? (height-text_height)*.5f+lh+gap : (height-vh)*.5f};
}
