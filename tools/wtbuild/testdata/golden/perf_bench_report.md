| run | frames | avg fps | 1%-low | p50 / p90 / p99 ms | over 50 / 100 ms | at 60 | GPU ms p50 / p90 / p99 | CPU ms/frame: main, render, MainJob1, MainDraw | load, GPU busy before | bound |
|---|---|---|---|---|---|---|---|---|---|---|
| a_shibuya | 900 | 56.4 | 18.2 | 16.2 / 18.5 / 55.0 | 10 / 1 | 89.1 % | 3.2 / 3.2 / 14.5 | 3.2, 3.2, 1.2, 0.6 | 4.30-5.37, 16 % | CPU side (GPU in 23 % of late frames; top thread main 3.2 ms) |
| b_cutscene | 3121 | 60.2 | 60.2 | 16.6 / 16.6 / 16.6 | 0 / 0 | 100.0 % | 0.0 / 0.0 / 0.0 | 4.0, 0.0, 3.0, 0.0 | None-None, - | - |
| c_tokyo | 300 | 48.8 | 40.0 | 25.0 / 25.0 / 25.0 | 0 / 0 | 50.0 % | 15.0 / 15.0 / 15.0 | 0.0, 0.0, 0.0, 0.0 | 4.25-None, 16 % | GPU (100 % of late frames) |
| d_cutscene | 0 | 0.0 | 0.0 | 0.0 / 0.0 / 0.0 | 0 / 0 | 0.0 % | 0.0 / 0.0 / 0.0 | 0.0, 0.0, 0.0, 0.0 | 1.00-None, 3 % | CPU side (GPU in 0 % of late frames; top thread ? 0.0 ms) |
a_shibuya: other GPU users at the start: 2 steam, 3 RDD Process, 5nospace
