(require 'term)

(defvar conpty-bridge-exe (expand-file-name "~/c-projects/conpty-emacs/main.exe")
  "Path to bridge CONPTY.")

(defun conpty-term-exec-1 (name buffer command switches)
  "Replaces term-exec-1: spawns via CONPTY bridge."
  (let* ((cols (if (and (boundp 'term-width) (> term-width 0)) term-width 80))
         (rows (if (and (boundp 'term-height) (> term-height 0)) term-height 24))
         (process-environment (cons "TERM=xterm-256color" process-environment)))
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

(defun conpty-term (program)
  "Opens a term running PROGRAM through CONPTY bridge."
  (interactive
   (list (read-string "Program: " "C:/Program Files/Git/bin/bash.exe")))
  (advice-add 'term-exec-1 :override #'conpty-term-exec-1)
  (unwind-protect
      (let ((buf (make-term "conpty" program)))
        (with-current-buffer buf
          (term-char-mode))
        (pop-to-buffer buf))
    (advice-remove 'term-exec-1 #'conpty-term-exec-1)))
