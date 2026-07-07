tell application "Finder"
    tell disk "sesivo"
        open
        set current view of container window to icon view
        set the bounds of container window to {100, 100, 700, 425}

        set viewOptions to the icon view options of container window
        set arrangement of viewOptions to not arranged
        set icon size of viewOptions to 96
        set text size of viewOptions to 16

        set position of item "Sesivo.app" of container window to {170, 100}
        set position of item "Applications" of container window to {430, 100}

        close
        open
        update without registering applications
        delay 1
    end tell
end tell
