;; reindent.el — batch reindent

;; Where this file lives
(defvar reindent-dir (file-name-directory (or load-file-name buffer-file-name)))
(add-to-list 'load-path reindent-dir)

;; Prefer compiled if available; be quiet
(load "xdr-mode" nil 'nomessage)

(defvar reindent-start-dir default-directory)

(defun reindent-quietly-one (file)
  (let* ((abs (expand-file-name file reindent-start-dir))
         ;; Minimize per-file overhead
         (vc-handled-backends nil)
         (create-lockfiles nil)
         (backup-inhibited t)
         (auto-save-default nil)
         (inhibit-message t)
         (message-log-max nil))
    ;; Keep rel paths stable (don’t inherit last buffer’s dir)
    (let ((default-directory reindent-start-dir))
      ;; Visit without selecting to avoid window setup
      (let ((buf (find-file-noselect abs)))
        (with-current-buffer buf
          ;; Font-lock isn’t active in --batch, but ensure it’s off
          (when (bound-and-true-p font-lock-mode) (font-lock-mode -1))
          (indent-region (point-min) (point-max))
          ;; Strip trailing whitespace (and trailing blank lines at EOF).
          (delete-trailing-whitespace)
          ;; Delete blank lines at the start of the buffer (a leading run
          ;; has no preceding line terminator, so the interior pattern
          ;; below would not see it), then collapse interior runs of
          ;; consecutive blank lines into a single one.
          (goto-char (point-min))
          (when (looking-at "\n+")
            (replace-match ""))
          (while (re-search-forward "\n\\{3,\\}" nil t)
            (replace-match "\n\n"))
          (when (buffer-modified-p)
            (save-buffer)))
        (kill-buffer buf)))))

;; One big GC stretch, and avoid file-name handlers during processing
(let ((gc-cons-threshold most-positive-fixnum)
      (gc-cons-percentage 0.6)
      (file-name-handler-alist nil))
  (dolist (f command-line-args-left)
    (reindent-quietly-one f)))
