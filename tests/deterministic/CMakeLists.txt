find_package(Qt6 REQUIRED COMPONENTS Core Test Widgets)

add_executable(bandcamp_deterministic_tests
    bandcamptests.cpp
    bandcampparsertests.cpp
    bandcamppostprocessortests.cpp
    bandcampcontracttests.cpp
    bandcampattemptsupervisortests.cpp
    bandcampdiagnosticstests.cpp
    bandcampclienttests.cpp
    bandcamporchestratortests.cpp
    bandcampresolutionservicetests.cpp
    bandcampfooyintracktests.cpp
    bandcampimportpipelinetests.cpp
    bandcamprefreshcontrollertests.cpp
    bandcampplaybacktests.cpp
)

target_link_libraries(bandcamp_deterministic_tests PRIVATE bandcamp_fooyin bandcamp_core Fooyin::Core Qt6::Core Qt6::Test)
set_target_properties(bandcamp_deterministic_tests PROPERTIES AUTOMOC ON)

add_test(NAME bandcamp.deterministic COMMAND bandcamp_deterministic_tests)
set_tests_properties(bandcamp.deterministic PROPERTIES TIMEOUT 300)

add_executable(bandcamp_progress_dialog_tests
    bandcampprogressdialogtests.cpp
    ${BANDCAMP_SRC_DIR}/bandcampprogressdialog.cpp
    ${BANDCAMP_SRC_DIR}/bandcampprogressdialog.h
)
target_link_libraries(bandcamp_progress_dialog_tests PRIVATE bandcamp_fooyin Qt6::Widgets Qt6::Test)
set_target_properties(bandcamp_progress_dialog_tests PROPERTIES AUTOMOC ON)
add_test(NAME bandcamp.progress-dialog COMMAND bandcamp_progress_dialog_tests)
set_tests_properties(bandcamp.progress-dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

add_executable(bandcamp_plugin_input_tests bandcampplugininputtests.cpp)
target_link_libraries(bandcamp_plugin_input_tests PRIVATE bandcamp_core Fooyin::Core Qt6::Core bandcamp_sanitize)
add_dependencies(bandcamp_plugin_input_tests bandcamp)
add_test(NAME bandcamp.plugin-input
    COMMAND bandcamp_plugin_input_tests "$<TARGET_FILE:bandcamp>"
)
set_tests_properties(bandcamp.plugin-input PROPERTIES TIMEOUT 120)

add_test(NAME bandcamp.import-pipeline
    COMMAND bandcamp_deterministic_tests --import-pipeline-tests
)
add_test(NAME bandcamp.import-controller
    COMMAND bandcamp_deterministic_tests --fooyin-track-tests
        importsIntoCapturedUiPlaylist
        reportsCreatedPlaylistForOpeningAfterImport
        importsPlaylistOnlyResidentMetadata
        reportsImportProgressAfterResolutionOrProbe
        countsFailedAndCancelledReleasesAsCompleted
        createsNewForForcedAndInitiallyUnsuitableDestinations
        preservesOrderedAlbumBlocksAndRepeatedScopes
        advancesAfterFailuresAndLeavesEmptyImportsUncreated
        stopsWhenDestinationChangesDuringPreparation
        waitsForGatewayBeforeFinishingResolutionImport
        cancellationWithdrawsPendingGatewayHandoff
        unavailableProbeFallsBackAndLogsOncePerTrack
        failedProbesStillAdmitResolvedScope
        capacityRejectedProbeRetriesWhenSlotFrees
        cancellationDuringProbeSkipsResolvedScope
        cancellationDuringProbePreservesAlreadyInsertedScopes
        deletingControllerFromPlaylistCallbackWithdrawsResolution
        cancellationDuringPlaylistCreationDoesNotInsert
        cancellationDuringInsertionDefersSummaryAndCountsAcceptedBlock
        deletingControllerFromIntegrationErrorWithdrawsResolution
)
set_tests_properties(bandcamp.import-pipeline bandcamp.import-controller PROPERTIES TIMEOUT 120)
