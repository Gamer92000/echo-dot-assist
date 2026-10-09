/* What differs between Echo models, as far as the daemon is concerned: identity, device nodes, stock file locations.
 * One definition per model in devices/<codename>/board.c, picked at link time by DEVICE in the Makefile (the PC builds link
 * the same one, so the protocol tests see the identity of the model they stand in for).  Everything else in src/hassmic is
 * meant to be the same on every model; when a port needs more than a different value here, add a field or a backend
 * (audio.h, wake.h) rather than an #ifdef. */
#ifndef BOARD_H
#define BOARD_H

struct board {
    /* identity towards Home Assistant and Music Assistant */
    const char *model;              /* ESPHome device info "model", Wyoming description */
    const char *project;            /* ESPHome "project_name" */
    const char *product;            /* Sendspin "product_name" */
    const char *default_name;       /* the model as people call it ("Echo Dot 3"); the name adds the MAC's end (main.c) */
    const char *codename;           /* devices/<codename>: names this model's bundle in a release (online updates) */

    /* inputs */
    const char *keypad;             /* input device with action and volume keys (-b overrides) */
    const char *keypad2;            /* second input device with more keys; NULL if one is enough */
    const char *privacy_state;      /* sysfs file, '1' = mics muted by the hardware latch; NULL if there is no latch */
    const char *privacy_input;      /* input device that reports changes of that latch; NULL if the keypad does */
    int grab_keys;                  /* 1: the keypad is hassmic's alone (EVIOCGRAB): Android's framework, where there is one,
                                       would act on the volume keys too and take its own volume off on top of ours */
    int privacy_latch;              /* 1: the sysfs state is the truth (read at start, on the mute key and once a second);
                                       0: nothing to read, presses of the keypad's mute key are counted from "unmuted" */

    /* Bluetooth: raw HCI (H4) character device, the init service that owns it in stock, where the address is stored */
    const char *bt_dev;
    const char *bt_service;
    const char *bt_mac;

    /* stock assets */
    const char *wake_id;            /* id of the firmware's own wake word model */
    const char *wake_manifest;
    const char *earcon_dir;         /* with trailing slash */
    /* where the firmware has no earcon folder: stock's sound name -> an entry of a stock app (APK) holding it, stored
     * uncompressed (sounds.c reads it out of the zip).  earcon_dir wins; {NULL}-terminated, or NULL for none */
    const struct board_sound { const char *name, *zip, *entry; } *earcon_zip;
    const char *thermal_type;       /* thermal zone reported as SoC temperature */
    const char *light_sensor[3];    /* files the stock light sensor HAL reads lux from, first that opens; {NULL}: none */
    int volume_steps;               /* volume_step-NN animations of ledcontroller */
};

extern const struct board board;
#endif
