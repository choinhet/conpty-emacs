(require 'term)

(defvar conpty-bridge-exe (expand-file-name "~/c-projects/conpty-emacs/main.exe")
  "Path to bridge CONPTY.")

(defun conpty--send-resize (&rest _)
  (when-let* ((proc (get-buffer-process (current-buffer)))
              (win  (get-buffer-window (current-buffer))))
    (let ((cols (window-body-width win))
          (rows (window-body-height win)))
      (process-send-string proc (format "\e_RESIZE;%d;%d\e\\" cols rows))
      (term-reset-size rows cols))))

(add-hook 'window-size-change-functions #'conpty--send-resize nil t)

(defun conpty-term-exec-1 (name buffer command switches)
  "Replaces term-exec-1: spawns via CONPTY bridge."
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

(defvar ps-exe "C:/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe")
(defvar bash-exe "C:/Program Files/Git/bin/bash.exe")

(defun conpty-term (program)
  "Opens a term running PROGRAM through CONPTY bridge."
  (interactive
   (list (read-string "Program: " bash-exe)))
  (advice-add 'term-exec-1 :override #'conpty-term-exec-1)
  (unwind-protect
      (let ((buf (make-term "conpty" program)))
        (with-current-buffer buf
          (setq-local locale-coding-system 'utf-8-unix)
          (term-char-mode)
          (conpty--send-resize))
        (pop-to-buffer buf))
    (advice-remove 'term-exec-1 #'conpty-term-exec-1)))
