;;; conpty.el --- Run console programs (Claude Code, shells) via a ConPTY bridge -*- lexical-binding: t; -*-

;; Bridges Windows console programs into Emacs term buffers through an
;; external ConPTY bridge (main.exe), which provides the pseudo-console
;; that Windows pipes alone can't.

(require 'term)

(defvar conpty-bridge-exe (expand-file-name "~/c-projects/conpty-emacs/main.exe")
  "Path to the ConPTY bridge executable.")

(defvar ps-exe "C:/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe"
  "Default program to run in the terminal.")

;;; ----------------------------------------------------------------------
;;; Resize
;;; ----------------------------------------------------------------------
;; The pipe to the bridge carries no window size, so we send the size
;; out-of-band as an APC sequence that the bridge intercepts. Driven by
;; window size changes; operates on the *conpty* buffer regardless of
;; which window currently has focus.

(defvar-local conpty--last-size nil
  "Last (cols . rows) sent to the bridge, to avoid redundant resizes.")

(defun conpty--send-resize (&rest _)
  "Send the current window size of the *conpty* buffer to the bridge."
  (when-let* ((buf (get-buffer "*conpty*"))
              (win (get-buffer-window buf))
              (proc (get-buffer-process buf)))
    (let* ((cols (window-body-width win))
           (rows (window-body-height win))
           (size (cons cols rows)))
      (unless (equal (buffer-local-value 'conpty--last-size buf) size)
        (with-current-buffer buf
          (setq conpty--last-size size))
        (process-send-string proc (format "\e_RESIZE;%d;%d\e\\" cols rows))
        ;; Reset term's internal size in the term window's context so it
        ;; redraws correctly even when another buffer has focus.
        (with-selected-window win
          (term-reset-size rows cols))))))

(add-hook 'window-size-change-functions #'conpty--send-resize)

;;; ----------------------------------------------------------------------
;;; Spawn
;;; ----------------------------------------------------------------------

(defun conpty-term-exec-1 (name buffer command switches)
  "Replace `term-exec-1': spawn COMMAND through the ConPTY bridge.
Runs over a pipe (no PTY on Windows); the bridge provides the
pseudo-console. Decoding stays binary so term sees raw bytes for its
escape parsing; encoding is UTF-8 so typed characters reach the program."
  (let* ((cols (if (and (boundp 'term-width) (> term-width 0)) term-width 80))
         (rows (if (and (boundp 'term-height) (> term-height 0)) term-height 24))
         (process-environment (cons "TERM=xterm-256color" process-environment))
         (proc
          (make-process
           :name name
           :buffer buffer
           :command (append (list conpty-bridge-exe
                                  command
                                  (number-to-string cols)
                                  (number-to-string rows))
                            switches)
           :coding 'binary
           :connection-type 'pipe
           :noquery t)))
    (set-process-coding-system proc 'binary 'utf-8-unix)
    proc))

(defun conpty-term ()
  "Open a terminal running `ps-exe' through the ConPTY bridge."
  (interactive)
  (advice-add 'term-exec-1 :override #'conpty-term-exec-1)
  (unwind-protect
      (let ((buf (make-term "conpty" ps-exe)))
        (with-current-buffer buf
          ;; term decodes display text with locale-coding-system; force
          ;; UTF-8 so box-drawing characters render instead of mojibake.
          (setq-local locale-coding-system 'utf-8-unix)
          (term-char-mode)
          (conpty--send-resize))
        (switch-to-buffer buf))
    (advice-remove 'term-exec-1 #'conpty-term-exec-1)))

;;; ----------------------------------------------------------------------
;;; Evil integration
;;; ----------------------------------------------------------------------
;; Insert state -> char mode: keys go to the program (interaction).
;; Normal state -> line mode: buffer is navigable, nothing is sent
;; (scrollback navigation, copying). ESC is reserved for the program
;; (Claude Code uses it to cancel), so C-g is the key that leaves char
;; mode into normal state.

(defun conpty--term-normal ()
  "Switch to line mode (navigation; no keys sent to the program)."
  (when (derived-mode-p 'term-mode)
    (term-line-mode)))

(defun conpty--term-insert ()
  "Switch to char mode (interact with the program)."
  (when (derived-mode-p 'term-mode)
    (term-char-mode)))

(add-hook 'term-mode-hook
          (lambda ()
            (display-line-numbers-mode -1)
            ;; Cap scrollback to the screen height so full-screen TUIs
            ;; (alt-screen) don't pile up repeated redraws. Toggle off
            ;; with `conpty-toggle-scrollback' for a normal shell.
            (setq-local term-buffer-maximum-size (window-body-height))
            (evil-insert-state)
            (add-hook 'evil-normal-state-entry-hook #'conpty--term-normal nil t)
            (add-hook 'evil-insert-state-entry-hook #'conpty--term-insert nil t)))

;; C-g leaves char mode into normal state; ESC still reaches the program.
(define-key term-raw-map (kbd "C-g")
            (lambda () (interactive) (evil-normal-state)))

;;; ----------------------------------------------------------------------
;;; Scrollback cap toggle
;;; ----------------------------------------------------------------------
;; Capped (= screen height): no alt-screen pile-up, best for TUIs.
;; Uncapped (= 0): full scrollback, best for a regular shell.

(defun conpty-toggle-scrollback ()
  "Toggle the scrollback cap in the current term buffer.
Capped keeps the buffer at one screen (good for full-screen programs
like Claude Code); uncapped keeps full history (good for shells)."
  (interactive)
  (unless (derived-mode-p 'term-mode)
    (user-error "Not in a term buffer"))
  (if (and term-buffer-maximum-size (> term-buffer-maximum-size 0))
      (progn
        (setq-local term-buffer-maximum-size 0)
        (message "conpty: scrollback uncapped (full history)"))
    (setq-local term-buffer-maximum-size (window-body-height))
    (message "conpty: scrollback capped to screen (no pile-up)")))

;;; ----------------------------------------------------------------------
;;; OSC title stripping
;;; ----------------------------------------------------------------------
;; term.el doesn't consume OSC title sequences (ESC ] 0 ; ... BEL/ST)
;; and prints them as literal "]0;..." junk. Strip them before term
;; processes its output.

(defun conpty--strip-osc (string)
  "Remove OSC title sequences from STRING."
  (replace-regexp-in-string "\e\\][0-9]*;[^\a\e]*\\(?:\a\\|\e\\\\\\)" "" string))

(advice-add 'term-emulate-terminal :filter-args
            (lambda (args)
              (list (car args) (conpty--strip-osc (cadr args)))))

(provide 'conpty)
;;; conpty.el ends here
