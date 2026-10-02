# The app's suites: core/ (model, mrkdwn, the fake workspace, the Slack
# service, the updater, audio, crashes), then one directory per app library
# and screen group, each only when that part of the app is built.
add_subdirectory(core)
foreach(part claude llm spell legacy)
    if(TARGET msga_app_${part})
        add_subdirectory(${part})
    endif()
endforeach()
foreach(group messages settings shell)
    if(TARGET msga_screens_${group})
        add_subdirectory(screens/${group})
    endif()
endforeach()
