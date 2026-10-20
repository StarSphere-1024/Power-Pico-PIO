Import("env")

# Native font checks use real LVGL without the STM32 LCD port.
env.AddBuildMiddleware(lambda env, node: None, "*/LVGL/porting/*")
