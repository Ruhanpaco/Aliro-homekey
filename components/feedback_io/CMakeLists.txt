idf_component_register(SRCS         "feedback_io.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     app_config
                       PRIV_REQUIRES access_control driver)
