# Main System UX + Multi-Question Capture Plan

## Goal

把 `main_system` 从“单题锁住式流程”升级成更像小型 cloud client：

```text
开机可拍照
也可进入 sidebar 查看旧题
拍照/上传/AI processing/rendering 时不锁住 UI
一次可以拍多题
每题独立上传、独立 solve、独立 polling
先完成的题可先看，后完成的题只更新 sidebar，不抢屏
```

## Problems To Fix

### 1. OLED answer view flicker

当前 `loop()` 每 20ms 都调用 `renderScreen()`。
`renderBitmapBlock()` 每次都会 `clearBuffer/sendBuffer`，所以 answer bitmap 页一直重刷，肉眼看到频闪。

Fix:

- 加 `screenDirty` flag。
- 按键、状态变更、job 状态更新时才标记 dirty。
- bitmap view 不再每 20ms 重绘。

### 2. Left button not sensitive

可能有硬件焊接因素，但软件可以改善：

- 降低 debounce。
- 加 repeat support：按住一段时间后可重复触发。
- 先对所有按钮统一改善，尤其 left/right scroll。

### 3. Sidebar 中文 preview 乱码

sidebar 用 `u8g2_font_6x10_tf`，不支持中文。

Fix:

- Sidebar 不显示 answer preview。
- 只显示：

```text
>Q1 READY
 Q2 PROC
```

状态：

```text
UPLD / PROC / REND / READY / ERR
```

### 4. Sidebar/page button sometimes hard to leave/enter

当前 `actionPage()` 在 `solveRunning` 时直接 return，会导致 AI running 时进不了 sidebar。

Fix:

- Page button 不再因为 background jobs 而禁用。
- 任何时候可以打开 sidebar。
- 在 sidebar 按 enter 进入选中题。

### 5. Capture / processing should not lock browsing

当前 global `solveRunning` 锁住 camera/page，并且 solve 完成后强制 `screenMode = MODE_VIEW`。

Fix:

- 用 `activeSolveJobs` counter 替代单一 `solveRunning`。
- 每题 background task 独立。
- job 完成时只更新对应 history item。
- 如果用户正在看该题或没有选择任何题，才自动显示第一个完成的题；否则不抢屏。

### 6. Multi-question capture

需求：

- 进入 capture 后拍 Q1。
- 按上下左右任意键进入下一个 draft question。
- 拍 Q2/Q3...
- 按 enter 一次提交所有 draft questions。
- 每个 draft question 独立上传、独立 `/solve`、独立 `/question` polling。

Implementation:

- 新增 `DraftQuestion`：

```cpp
struct DraftQuestion {
  int id;
  std::vector<PendingPhoto> photos;
};
```

- `draftQuestions` 保存这批待提交题。
- `activeDraftIndex` 指向当前正在拍的题。
- Direction key in capture mode calls `moveToNextDraftQuestion()`.
- Enter calls `startSolveFromDrafts()`，为每个有照片的 draft 创建独立 `SolveJob` task。

## Server Impact

当前 server API 已经以 `deviceId + questionNo` 为隔离单位：

- `/upload-photo?question_id=&index=`
- `/solve` body includes `question_id` + `photo_ids`
- `/question?question_id=`
- `/question-block?question_id=&index=`

所以这次不需要 DB migration，也不需要新增 endpoint。

需要 server 注意的只是：

- 多个 question 同时 request 时都能走 `after()`。
- logs 里 question_id 分开。

## Main System Target UX

### Ready

```text
CAM: new photo
PAGE: sidebar
```

### Capture

```text
Q3 [1/3] photos
CAM shot ENT send
```

Direction key:

```text
Q4 [0/3] photos
CAM shot ENT send
```

### Sidebar

```text
>Q1 READY
 Q2 PROC
```

### Viewing answer

- Up/down: block/page or vertical movement for 64px formula.
- Left/right: horizontal scroll.
- Page: sidebar.
- Enter: blank/unblank OLED answer.
- Camera: go capture new batch.

## Validation

- `platformio run -e main_system`
- Optional `platformio run -e ai_solve_test` should remain OK.
- Manual test:
  1. Boot → sidebar works.
  2. Camera → capture one question → enter → can go sidebar while processing.
  3. Capture two questions in one batch → enter → two sidebar rows show `UPLD/PROC/READY`.
  4. First completed answer does not steal screen if user is viewing another answer.
  5. Answer view has no continuous flicker.

