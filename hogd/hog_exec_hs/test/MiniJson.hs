-- | A small JSON reader for test vector files (Wycheproof). It accepts
-- RFC 8259 JSON; strings are returned with escapes decoded, and numbers as
-- their source text.
module MiniJson
  ( Value (..)
  , parseJson
  , field
  , fieldMaybe
  , str
  , arr
  , strs
  ) where

import Data.Char (chr, isDigit, isHexDigit, isSpace)
import Numeric (readHex)

data Value
  = Object [(String, Value)]
  | Array [Value]
  | String String
  | Number String
  | Bool Bool
  | Null
  deriving (Eq, Show)

parseJson :: String -> Either String Value
parseJson s = case value (skip s) of
  Right (v, rest) | all isSpace rest -> Right v
  Right _ -> Left "trailing data"
  Left e -> Left e

type P a = String -> Either String (a, String)

skip :: String -> String
skip = dropWhile isSpace

value :: P Value
value ('{' : r) = object (skip r)
value ('[' : r) = array (skip r)
value ('"' : r) = do
  (s, r') <- string r
  pure (String s, r')
value ('t' : 'r' : 'u' : 'e' : r) = Right (Bool True, r)
value ('f' : 'a' : 'l' : 's' : 'e' : r) = Right (Bool False, r)
value ('n' : 'u' : 'l' : 'l' : r) = Right (Null, r)
value s@(c : _)
  | c == '-' || isDigit c =
      let (n, r) = span (\x -> isDigit x || x `elem` ("+-.eE" :: String)) s in Right (Number n, r)
value s = Left ("unexpected input: " ++ take 20 s)

object :: P Value
object ('}' : r) = Right (Object [], r)
object s = go [] s
  where
    go acc ('"' : r) = do
      (k, r1) <- string r
      case skip r1 of
        ':' : r2 -> do
          (v, r3) <- value (skip r2)
          case skip r3 of
            ',' : r4 -> go ((k, v) : acc) (skip r4)
            '}' : r4 -> Right (Object (reverse ((k, v) : acc)), r4)
            _ -> Left "expected , or }"
        _ -> Left "expected :"
    go _ _ = Left "expected key"

array :: P Value
array (']' : r) = Right (Array [], r)
array s = go [] s
  where
    go acc r = do
      (v, r1) <- value r
      case skip r1 of
        ',' : r2 -> go (v : acc) (skip r2)
        ']' : r2 -> Right (Array (reverse (v : acc)), r2)
        _ -> Left "expected , or ]"

string :: P String
string = go []
  where
    go acc ('"' : r) = Right (reverse acc, r)
    go acc ('\\' : c : r) = case c of
      '"' -> go ('"' : acc) r
      '\\' -> go ('\\' : acc) r
      '/' -> go ('/' : acc) r
      'b' -> go ('\b' : acc) r
      'f' -> go ('\f' : acc) r
      'n' -> go ('\n' : acc) r
      'r' -> go ('\r' : acc) r
      't' -> go ('\t' : acc) r
      'u'
        | (h, r') <- splitAt 4 r
        , length h == 4
        , all isHexDigit h
        , [(n, "")] <- readHex h ->
            go (chr n : acc) r'
      _ -> Left "bad escape"
    go acc (c : r) = go (c : acc) r
    go _ [] = Left "unterminated string"

fieldMaybe :: String -> Value -> Maybe Value
fieldMaybe k (Object kvs) = lookup k kvs
fieldMaybe _ _ = Nothing

field :: String -> Value -> Value
field k v = maybe (error ("missing field " ++ k)) id (fieldMaybe k v)

str :: Value -> String
str (String s) = s
str v = error ("not a string: " ++ take 40 (show v))

arr :: Value -> [Value]
arr (Array vs) = vs
arr v = error ("not an array: " ++ take 40 (show v))

strs :: Value -> [String]
strs = map str . arr
