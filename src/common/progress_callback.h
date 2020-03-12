#pragma once
#include "string.h"
#include "types.h"

class ByteStream;

class ProgressCallback
{
public:
  virtual ~ProgressCallback();

  virtual void PushState() = 0;
  virtual void PopState() = 0;

  virtual bool IsCancelled() const = 0;
  virtual bool IsCancellable() const = 0;

  virtual void SetCancellable(bool cancellable) = 0;

  virtual void SetStatusText(const char* text) = 0;
  virtual void SetProgressRange(u32 range) = 0;
  virtual void SetProgressValue(u32 value) = 0;
  virtual void IncrementProgressValue() = 0;

  void SetFormattedStatusText(const char* Format, ...);

  virtual void DisplayError(const char* message) = 0;
  virtual void DisplayWarning(const char* message) = 0;
  virtual void DisplayInformation(const char* message) = 0;
  virtual void DisplayDebugMessage(const char* message) = 0;

  virtual void ModalError(const char* message) = 0;
  virtual bool ModalConfirmation(const char* message) = 0;
  virtual u32 ModalPrompt(const char* message, u32 num_options, ...) = 0;

  void DisplayFormattedError(const char* format, ...);
  void DisplayFormattedWarning(const char* format, ...);
  void DisplayFormattedInformation(const char* format, ...);
  void DisplayFormattedDebugMessage(const char* format, ...);
  void DisplayFormattedModalError(const char* format, ...);
  bool DisplayFormattedModalConfirmation(const char* format, ...);

  void UpdateProgressFromStream(ByteStream* stream);

public:
  static ProgressCallback* NullProgressCallback;
};

class BaseProgressCallback : public ProgressCallback
{
public:
  BaseProgressCallback();
  virtual ~BaseProgressCallback();

  virtual void PushState() override;
  virtual void PopState() override;

  virtual bool IsCancelled() const override;
  virtual bool IsCancellable() const override;

  virtual void SetCancellable(bool cancellable) override;
  virtual void SetStatusText(const char* text) override;
  virtual void SetProgressRange(u32 range) override;
  virtual void SetProgressValue(u32 value) override;
  virtual void IncrementProgressValue() override;

protected:
  struct State
  {
    State* next_saved_state;
    String status_text;
    u32 progress_range;
    u32 progress_value;
    u32 base_progress_value;
    bool cancellable;
  };

  bool m_cancellable;
  bool m_cancelled;
  String m_status_text;
  u32 m_progress_range;
  u32 m_progress_value;

  u32 m_base_progress_value;

  State* m_saved_state;
};

class ConsoleProgressCallback : public BaseProgressCallback
{
public:
  static const u32 COLUMNS = 78;

public:
  ConsoleProgressCallback();
  ~ConsoleProgressCallback();

  virtual void PushState() override;
  virtual void PopState() override;

  virtual void SetCancellable(bool cancellable) override;
  virtual void SetStatusText(const char* text) override;
  virtual void SetProgressRange(u32 range) override;
  virtual void SetProgressValue(u32 value) override;

  virtual void DisplayError(const char* message) override;
  virtual void DisplayWarning(const char* message) override;
  virtual void DisplayInformation(const char* message) override;
  virtual void DisplayDebugMessage(const char* message) override;

  virtual void ModalError(const char* message) override;
  virtual bool ModalConfirmation(const char* message) override;
  virtual u32 ModalPrompt(const char* message, u32 num_options, ...) override;

private:
  void Clear();
  void Redraw(bool update_value_only);

  float m_last_percent_complete;
  u32 m_last_bar_length;
};
