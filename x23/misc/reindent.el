;; reindent.el

(defvar reindent-dir (file-name-directory (or load-file-name buffer-file-name)))
(add-to-list 'load-path reindent-dir)

;; Prefer .eln/.elc if available, and be quiet
(load "xdr-mode" nil 'nomessage)

(defvar reindent-start-dir default-directory)

(defun reindent-quietly (file)
  (let ((abs (expand-file-name file reindent-start-dir)))
    (let ((default-directory reindent-start-dir))
      (find-file abs))
    (let ((inhibit-message t) (message-log-max nil))
      (indent-region (point-min) (point-max)))
    (save-buffer)
    (kill-buffer (current-buffer))))

(dolist (f command-line-args-left)
  (reindent-quietly f))
