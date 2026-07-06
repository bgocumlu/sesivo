tell application "Finder"
    tell disk "sesivo"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set the bounds of container window to {360, 180, 900, 500}

        set viewOptions to the icon view options of container window
        set arrangement of viewOptions to not arranged
        set icon size of viewOptions to 96
        set text size of viewOptions to 13

        set position of item "Applications" of container window to {150, 150}
        set position of item "sesivo.app" of container window to {390, 150}

        close
        open
        update without registering applications
        delay 1
    end tell
end tell
