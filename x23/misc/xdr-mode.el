;;; xdr-mode.el --- Major mode for editing XDR (RFC4506/RFC5531) files -*- lexical-binding: t; -*-

;; Public domain

;; Author: Claude
;; Keywords: languages, xdr
;; Version: 1.0

;; This file is not part of GNU Emacs.

;;; Commentary:

;; Major mode for editing XDR (External Data Representation) files
;; as defined in RFC4506 and RFC5531. Derived from c++-mode with
;; custom indentation for unions and proper syntax highlighting.

;;; Code:

(require 'cc-mode)
(require 'cc-langs)
(require 'cc-fonts)

;; Define XDR as a new CC Mode language
(eval-and-compile
  (c-add-language 'xdr-mode 'c++-mode))

;; Define XDR-specific keywords and types
(defconst xdr-keywords
  '("typedef" "const" "enum" "struct" "union" "program" "version" 
    "case" "default" "switch")
  "XDR keywords.")

(defconst xdr-types
  '("void" "bool" "int" "unsigned" "hyper" "quadruple" "float" 
    "double" "string" "opaque")
  "XDR built-in types.")

;; Set up CC Mode language constants for XDR
(c-lang-defconst c-primitive-type-kwds
  xdr (append (c-lang-const c-primitive-type-kwds c++) xdr-types))

(c-lang-defconst c-type-modifier-kwds
  xdr nil)

(c-lang-defconst c-class-decl-kwds
  xdr '("struct" "union" "enum" "program" "version"))

(c-lang-defconst c-brace-list-decl-kwds
  xdr '("enum"))

;; Remove C++-specific keywords that don't apply to XDR
(c-lang-defconst c-protection-kwds
  xdr nil)

(c-lang-defconst c-other-block-decl-kwds
  xdr nil)

;; Define XDR-specific syntactic symbols for indentation
(defun xdr-lineup-union-case (langelem)
  "Line up case labels in XDR unions to column 0."
  (save-excursion
    (beginning-of-line)
    (when (looking-at "[ \t]*\\(case\\|default\\)")
      ;; Check if we're in a union by looking backwards
      (let ((case-pos (point))
            (in-union nil))
        (when (re-search-backward "{\\|}\\|\\bunion\\b" nil t)
          (cond
           ;; Found closing brace - not in union
           ((looking-at "}")
            nil)
           ;; Found opening brace - check if it's a union
           ((looking-at "{")
            (beginning-of-line)
            (when (re-search-backward "\\bunion\\b" (line-beginning-position) t)
              (setq in-union t)))
           ;; Found union keyword directly
           ((looking-at "\\bunion\\b")
            (setq in-union t))))
        (when in-union
          (- 0 (current-column)))))))

(defun xdr-lineup-union-close (langelem)
  "Line up closing braces of XDR unions to column 0."
  (save-excursion
    (beginning-of-line)
    (when (looking-at "[ \t]*}")
      ;; Move to the matching opening brace
      (goto-char (cdr langelem))
      (beginning-of-line)
      ;; Check if this is a union
      (when (re-search-backward "\\bunion\\b" (line-beginning-position) t)
        (- 0 (current-column))))))

;; Custom indentation setup
(defconst xdr-style
  '((c-basic-offset . 2)
    (c-comment-only-line-offset . 0)
    (c-offsets-alist . ((string . c-lineup-dont-change)
                        (c . c-lineup-C-comments)
                        (defun-open . 0)
                        (defun-close . 0)
                        (defun-block-intro . +)
                        (class-open . 0)
                        (class-close . 0)
                        (inline-open . 0)
                        (inline-close . 0)
                        (func-decl-cont . +)
                        (knr-argdecl-intro . +)
                        (knr-argdecl . 0)
                        (topmost-intro . 0)
                        (topmost-intro-cont . c-lineup-topmost-intro-cont)
                        (member-init-intro . +)
                        (member-init-cont . c-lineup-multi-inher)
                        (inher-intro . +)
                        (inher-cont . c-lineup-multi-inher)
                        (block-open . 0)
                        (block-close . 0)
                        (brace-list-open . 0)
                        (brace-list-close . 0)
                        (brace-list-intro . +)
                        (brace-list-entry . 0)
                        (statement . 0)
                        (statement-cont . +)
                        (statement-block-intro . +)
                        (statement-case-intro . +)
                        (statement-case-open . 0)
                        (substatement . +)
                        (substatement-open . 0)
                        (substatement-label . 0)
                        (case-label . 0)
                        (access-label . -)
                        (label . 2)
                        (do-while-closure . 0)
                        (else-clause . 0)
                        (catch-clause . 0)
                        (comment-intro . c-lineup-comment)
                        (arglist-intro . +)
                        (arglist-cont . 0)
                        (arglist-cont-nonempty . c-lineup-arglist)
                        (arglist-close . +)
                        (stream-op . c-lineup-streamop)
                        (inclass . +)
                        (cpp-macro . -1000)
                        (cpp-macro-cont . +)
                        (friend . 0)
                        (objc-method-intro . -1000)
                        (objc-method-args-cont . c-lineup-ObjC-method-args)
                        (objc-method-call-cont . c-lineup-ObjC-method-call)
                        (extern-lang-open . 0)
                        (extern-lang-close . 0)
                        (inextern-lang . +)
                        (namespace-open . 0)
                        (namespace-close . 0)
                        (innamespace . 0)
                        (template-args-cont . +)
                        (inlambda . c-lineup-inexpr-block)
                        (lambda-intro-cont . +)
                        (inexpr-statement . +)
                        (inexpr-class . +))))
  "XDR indentation style.")

;; Font lock keywords for syntax highlighting
(defconst xdr-font-lock-keywords
  (append
   c++-font-lock-keywords-1
   (list
    ;; XDR keywords
    (cons (regexp-opt xdr-keywords 'words) 'font-lock-keyword-face)
    ;; XDR built-in types
    (cons (regexp-opt xdr-types 'words) 'font-lock-type-face)
    ;; TRUE and FALSE
    '("\\b\\(TRUE\\|FALSE\\)\\b" . font-lock-constant-face)
    ;; Numbers (including hex with 0x prefix)
    '("\\b\\(0[xX][0-9a-fA-F]+\\|[0-9]+\\)\\b" . font-lock-constant-face)
    ;; Type definitions (after typedef)
    '("typedef\\s-+\\(?:\\sw+\\s-*[<>]*\\s-+\\)*\\(\\sw+\\)" 1 font-lock-type-face)
    ;; Enum/struct/union names
    '("\\(?:enum\\|struct\\|union\\)\\s-+\\(\\sw+\\)" 1 font-lock-type-face)
    ;; Program and version names
    '("\\(?:program\\|version\\)\\s-+\\(\\sw+\\)" 1 font-lock-function-name-face)))
  "Font lock keywords for XDR mode.")

;; Syntax table modifications
(defvar xdr-mode-syntax-table
  (let ((table (make-syntax-table c++-mode-syntax-table)))
    ;; Make '<' and '>' behave like brackets for array syntax
    (modify-syntax-entry ?< "(>" table)
    (modify-syntax-entry ?> ")<" table)
    table)
  "Syntax table for XDR mode.")

;; Special handling for union case indentation
(defun xdr-looking-at-union-case ()
  "Return t if current line is a case/default in a union."
  (save-excursion
    (beginning-of-line)
    (and (looking-at "^[ \t]*\\(case\\|default\\)")
         (let ((orig-point (point))
               (found-union nil))
           (when (re-search-backward "{\\|}\\|\\bunion\\b" nil t)
             (cond
              ((looking-at "}")
               nil)
              ((looking-at "{")
               (save-excursion
                 (beginning-of-line)
                 (when (re-search-backward "\\bunion\\b" (line-beginning-position) t)
                   (setq found-union t))))
              ((looking-at "\\bunion\\b")
               (setq found-union t))))
           found-union))))

;; Override the indentation function to handle union cases specially
(defun xdr-indent-line ()
  "Indent current line as XDR code."
  (interactive)
  (if (xdr-looking-at-union-case)
      ;; For union case labels, indent to column 0
      (let ((cur-indent (current-indentation)))
        (when (/= cur-indent 0)
          (beginning-of-line)
          (delete-horizontal-space)
          (indent-to 0))
        (when (= (point) (line-beginning-position))
          (back-to-indentation)))
    ;; For everything else, use standard CC Mode indentation
    (c-indent-line)))

;; Define the major mode keymap
(defvar xdr-mode-map
  (let ((map (make-sparse-keymap)))
    (set-keymap-parent map c++-mode-map)
    map)
  "Keymap for XDR mode.")

;; Define the major mode
;;;###autoload
(defun xdr-mode ()
  "Major mode for editing XDR (External Data Representation) files.

This mode is derived from c++-mode but provides proper indentation
for XDR union constructs and syntax highlighting for XDR-specific
keywords and types.

\\{xdr-mode-map}"
  (interactive)
  (kill-all-local-variables)
  
  ;; Initialize c++-mode without running its hooks
  (let ((c++-mode-hook nil))
    (c++-mode))
  
  ;; Set up XDR-specific features
  (setq major-mode 'xdr-mode)
  (setq mode-name "XDR")
  (use-local-map xdr-mode-map)
  (set-syntax-table xdr-mode-syntax-table)
  
  ;; Apply XDR style
  (c-add-style "xdr" xdr-style t)
  
  ;; Set up font locking
  (setq font-lock-defaults
        '(xdr-font-lock-keywords nil nil nil nil))
  
  ;; Use custom indentation that handles union cases
  (setq-local indent-line-function #'xdr-indent-line)
  
  ;; Set up comment syntax
  (setq-local comment-start "/* ")
  (setq-local comment-end " */")
  (setq-local comment-start-skip "/\\*+[ \t]*\\|//+[ \t]*")
  
  ;; Run XDR mode hooks
  (run-mode-hooks 'xdr-mode-hook))

;; Add auto-mode-alist entry
;;;###autoload
(add-to-list 'auto-mode-alist '("\\.x\\'" . xdr-mode))

;; Provide the feature
(provide 'xdr-mode)

;;; xdr-mode.el ends here
