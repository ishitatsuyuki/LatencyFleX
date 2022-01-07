## Profiling

[Perfetto](https://perfetto.dev/) can be used to gather a detailed picture of execution.

The steps to use perfetto for profiling are:
1. Build perfetto from sources available at layer/subprojects/perfetto following
   [this guide](https://perfetto.dev/docs/quickstart/linux-tracing).
2. `cd layer/subprojects/perfetto` and run the helper script. latencyflex.cfg is available in this docs directory.
   ```shell
   tools/tmux -c path/to/latencyflex.cfg -C out/linux -n
   ```
3. Launch your game. When you are ready to capture, switch to the bottom tmux pane and press enter to run the supplied
   perfetto CLI invocation.
   
   The capture lasts 60 seconds by default, but you can interrupt as you want. You can also modify the command line to
   sleep for a delay before capturing.

   When capturing multiple sessions, make sure you change the output file names specified in `-o` of the perfetto CLI
   invocation.
4. Go to https://ui.perfetto.dev and view the trace by selecting "Open Trace File" and navigating into `/tmp/perfetto.XXXXXX`.