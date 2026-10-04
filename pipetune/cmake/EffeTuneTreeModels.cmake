# Run in the upstream directory after its model target exists so the original
# and adapted generation rules share one CMake dependency scope.
cmake_language(DEFER CALL pipetune_configure_effetune_tree_models
               "${PIPETUNE_NATIVE_PROCESSOR}")
