/**
  ******************************************************************************
  * @file    app_loop.h
  * @brief   アプリのメインループ【コア層・基板非依存】。
  ******************************************************************************
  */
#ifndef APP_LOOP_H
#define APP_LOOP_H

#ifdef __cplusplus
extern "C" {
#endif

/* ACTIVE/IDLEステートマシンを回し続ける。戻ってこない。
 * 呼ぶ前に Board_Init() / Sensors_Init() / Audio_Init() を済ませておくこと。 */
void App_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOOP_H */
