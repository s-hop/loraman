idf_component_register(SRCS "main.cpp" "boot.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES
                        loraman
                        console
                        nvs_flash
                        freertos
                        esp_system
                        driver
                        jgromes__radiolib)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)