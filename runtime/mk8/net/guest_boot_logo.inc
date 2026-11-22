    if (mk8_guest_lan_hide_boot_logo()) {
        // Reuse the boot layout's pane lookup. Stock immediately reuses this string for N_Capture_00.
        c->x[0] = LD64(c->x[19] + 0x1d0);
        c->x[1] = c->sp + 8;
        ST64(c->sp + 16, 0x7000ee914bull);  // N_Logo_00 in MK8 4.0.1
        f_70008ad8e8(c);
        c->x[1] = 0;
        f_70008ade74(c);  // SetVisible(false), also accepts a missing pane.
    }
