(defun conpty-filter (process output)
  (message "OUTPUT: %S" output)
  (with-current-buffer (process-buffer process)
    (let ((inhibit-read-only t))
      (goto-char (point-max))
      (insert output))))

(defun conpty-start (program cols rows &optional args)
  "Spawns a conpty process with PROGRAM, with size equals COLS x ROWS"
  (let ((process (make-process
                  :name "conpty"
                  :buffer "*conpty*"
                  :command (append
                            (list (expand-file-name "~/c-projects/conpty-emacs/main.exe")
                                  program
                                  (number-to-string cols)
                                  (number-to-string rows))
                            args)
                  :coding 'binary
                  :connection-type 'pipe
                  :filter 'conpty-filter)))
    process))

(defun conpty-send-string (process string)
  "Sends a STRING to the conpty process"
  (process-send-string process string))

(defun test-send ()
  (interactive)
  (let ((proc (get-process "conpty")))
    (message "Sending to process: %s" (process-status proc))
    (conpty-send-string proc "echo 'hello'\r\n")))

(process-live-p (get-process "conpty"))

(defun test-start ()
  (interactive)
  (conpty-start  "C:/Program Files/Git/bin/bash.exe" 80 24)
  (pop-to-buffer "*conpty*"))

