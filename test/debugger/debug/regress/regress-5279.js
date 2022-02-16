// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


var Debug = debug.Debug;

Debug.setListener(() => undefined);

function f() {
  const myObj = {};

  for (let i = 0; i < 10; i++) {
    %OptimizeOsr(0, "concurrent");
    %ScheduleBreak();
    %PrepareFunctionForOptimization(f);
  }
}
%PrepareFunctionForOptimization(f);
f()
