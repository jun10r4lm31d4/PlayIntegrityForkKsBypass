# Allow a scripts-only mode on older Android without Zygisk
if [ "$API" -lt 26 -o -f /data/adb/modules/playintegrityfix/scripts-only-mode ]; then
    ui_print "! Installing global scripts only for Android < 8.0; Zygisk attestation fallback and device spoofing disabled"
    touch $MODPATH/scripts-only-mode
    rm -rf $MODPATH/classes.dex $MODPATH/common_setup.sh $MODPATH/custom.pif.json \
        $MODPATH/example.app_replace.list $MODPATH/example.pif.json $MODPATH/migrate.sh $MODPATH/zygisk \
        /data/adb/modules/playintegrityfix/custom.app_replace.list \
        /data/adb/modules/playintegrityfix/custom.pif.json /data/adb/modules/playintegrityfix/system
fi

# Copy any disabled app files to updated module
if [ -d /data/adb/modules/playintegrityfix/system ]; then
    ui_print "- Restoring disabled ROM apps configuration"
    cp -arf /data/adb/modules/playintegrityfix/system $MODPATH
fi

# Copy any supported custom files to updated module
for FILE in custom.app_replace.list custom.pif.json keybox.xml; do
    if [ -f "/data/adb/modules/playintegrityfix/$FILE" ]; then
        ui_print "- Restoring $FILE"
        cp -af /data/adb/modules/playintegrityfix/$FILE $MODPATH/$FILE
    fi
done

# Warn if potentially conflicting modules are installed
if [ -d /data/adb/modules/MagiskHidePropsConf ]; then
    ui_print "! MagiskHidePropsConfig (MHPC) module may cause issues with PIF"
fi

# Run common tasks for installation and boot-time
[ -d "$MODPATH/zygisk" ] && . $MODPATH/common_setup.sh

# Migrate custom.pif.json to latest defaults if needed
if [ -f "$MODPATH/custom.pif.json" ] && ! grep -q "api_level" $MODPATH/custom.pif.json; then
    ui_print "- Running migration script on custom.pif.json:"
    ui_print " "
    chmod 755 $MODPATH/migrate.sh
    sh $MODPATH/migrate.sh install $MODPATH/custom.pif.json
    ui_print " "
fi

# Clean up any leftover files from previous deprecated methods
rm -f /data/data/com.google.android.gms/cache/pif.prop /data/data/com.google.android.gms/pif.prop \
    /data/data/com.google.android.gms/cache/pif.json /data/data/com.google.android.gms/pif.json
