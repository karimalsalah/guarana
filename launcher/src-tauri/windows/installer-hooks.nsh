; Installer hooks (bundle > windows > nsis > installerHooks in tauri.conf.json).
;
; The installer is per user (installMode currentUser), so Tauri's template writes the
; fileAssociations and the guarana:// scheme under HKCU\Software\Classes (SHCTX), with no admin.
; These hooks add what the template leaves out: each extension lists Guarana under
; OpenWithProgids, so "Open with" and Settings > Default apps offer it even where the user picked
; another program, and Explorer is told the associations changed instead of waiting for a restart.
; Keep the ProgIDs in step with the "name" of each entry in fileAssociations.

!macro GUARANA_OPEN_WITH EXT
  WriteRegStr SHCTX "Software\Classes\.${EXT}\OpenWithProgids" "Guarana.${EXT}" ""
!macroend

; Only Guarana's value goes: other programs list themselves in the same key.
!macro GUARANA_OPEN_WITH_REMOVE EXT
  DeleteRegValue SHCTX "Software\Classes\.${EXT}\OpenWithProgids" "Guarana.${EXT}"
!macroend

!macro NSIS_HOOK_POSTINSTALL
  !insertmacro GUARANA_OPEN_WITH "nsp"
  !insertmacro GUARANA_OPEN_WITH "xci"
  !insertmacro GUARANA_OPEN_WITH "nup"
  !insertmacro GUARANA_OPEN_WITH "nca"
  !insertmacro UPDATEFILEASSOC
!macroend

!macro NSIS_HOOK_POSTUNINSTALL
  !insertmacro GUARANA_OPEN_WITH_REMOVE "nsp"
  !insertmacro GUARANA_OPEN_WITH_REMOVE "xci"
  !insertmacro GUARANA_OPEN_WITH_REMOVE "nup"
  !insertmacro GUARANA_OPEN_WITH_REMOVE "nca"
  !insertmacro UPDATEFILEASSOC
!macroend
