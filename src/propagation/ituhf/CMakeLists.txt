add_library(fobos_hf_prediction STATIC
    hfprediction.h hfprediction.cpp hfimagevalidation_p.h hfpredictiondialog.h hfpredictiondialog.cpp)
set_target_properties(fobos_hf_prediction PROPERTIES AUTOMOC ON)
target_compile_features(fobos_hf_prediction PUBLIC cxx_std_17)
target_include_directories(fobos_hf_prediction PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(fobos_hf_prediction PUBLIC Qt5::Core Qt5::Widgets)
