;; reindent.el

;; Find the directory this file lives in
(defvar reindent-dir
  (file-name-directory (or load-file-name buffer-file-name)))

;; Load xdr-mode.el from the same directory
(load-file (expand-file-name "xdr-mode.el" reindent-dir))

;; Process each file given on the command line
(mapc (lambda (file)
        (find-file file)
        ;; major mode activates automatically
        (indent-region (point-min) (point-max))
        (save-buffer))
      command-line-args-left)
