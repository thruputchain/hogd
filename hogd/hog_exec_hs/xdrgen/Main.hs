{-# LANGUAGE LambdaCase #-}

-- | xdrgen: compile x23-dialect XDR (RFC 4506 + RFC 5531 programs, with
-- x23's multi-argument procedures) into Haskell modules.
--
-- Usage: xdrgen OUTDIR file1.x file2.x ...
--
-- Each input file becomes module Hogd.Xdr.<CamelName>.  Representation:
--
--  * struct          -> record (DuplicateRecordFields, NoFieldSelectors)
--  * enum            -> open newtype over Int32 with pattern synonyms
--  * union           -> sum type, one constructor per arm; arms covering
--                       several cases (or default) also carry the
--                       discriminant
--  * typedef         -> type synonym plus put_/get_/cxsPut_/cxsGet_ codecs
--  * opaque/string   -> ByteString; T<> / T[n] -> [T]; T* -> Maybe T
--  * const           -> polymorphic numeric binding
--  * program         -> server record + dispatcher, and client call stubs
module Main (main) where

import Control.Monad
import Data.Char
import Data.List (intercalate, isPrefixOf, nub, sortOn)
import qualified Data.Map.Strict as M
import Data.Maybe
import qualified Data.Set as S
import System.Directory (createDirectoryIfMissing)
import System.Environment (getArgs)
import System.Exit
import System.FilePath
import System.IO
import GHC.IO.Encoding (setLocaleEncoding)
import Text.Parsec hiding (State)
import Text.Parsec.String (Parser)

------------------------------------------------------------------------------
-- AST

data Base
  = BInt
  | BUInt
  | BHyper
  | BUHyper
  | BBool
  | BFloat
  | BDouble
  | BNamed String
  | BOpaque
  | BString
  | BEnum [(String, String)]
  | BStruct [Decl]
  | BUnion UnionBody
  deriving (Show)

data Qual = QPlain | QPtr | QArray String | QVec (Maybe String)
  deriving (Show)

data Decl = DVoid | Decl {dBase :: Base, dId :: String, dQual :: Qual}
  deriving (Show)

data UnionBody = UnionBody {uSwType :: Base, uSwId :: String, uArms :: [Arm]}
  deriving (Show)

data Arm = Arm {armCases :: [String], armDefault :: Bool, armDecl :: Decl}
  deriving (Show)

data Proc = Proc {pName :: String, pRet :: Maybe Base, pArgs :: [Base], pNum :: String}
  deriving (Show)

data Vers = Vers {vName :: String, vNum :: String, vProcs :: [Proc]}
  deriving (Show)

data Def
  = DefTypedef Decl
  | DefEnum String [(String, String)]
  | DefStruct String [Decl]
  | DefUnion String UnionBody
  | DefConst String String
  | DefProgram String String [Vers]
  deriving (Show)

------------------------------------------------------------------------------
-- Lexing helpers

lexeme :: Parser a -> Parser a
lexeme p = p <* ws

ws :: Parser ()
ws = skipMany (void (oneOf " \t\r\n") <|> lineComment <|> blockComment <|> passthrough)
  where
    lineComment = try (string "//") >> void (manyTill anyChar (void newline <|> eof))
    blockComment = try (string "/*") >> void (manyTill anyChar (try (string "*/")))
    passthrough = do
      -- '%' lines are only passthrough at the start of a line, but treating
      -- every '%' as one is harmless for the inputs we compile.
      _ <- char '%'
      void (manyTill anyChar (void newline <|> eof))

sym :: String -> Parser String
sym s = lexeme (try (string s))

keywords :: [String]
keywords =
  [ "bool", "case", "const", "default", "double", "quadruple", "enum", "float", "hyper", "int", "opaque"
  , "string", "struct", "switch", "typedef", "union", "unsigned", "void", "program", "version", "namespace"
  ]

identRaw :: Parser String
identRaw = lexeme $ try $ do
  c <- letter <|> char '_'
  cs <- many (alphaNum <|> char '_')
  pure (c : cs)

ident :: Parser String
ident = try $ do
  s <- identRaw
  when (s `elem` keywords) $ unexpected ("keyword " ++ s)
  pure s

kw :: String -> Parser ()
kw s = void $ try $ do
  x <- identRaw
  if x == s then pure x else unexpected x

number :: Parser String
number = lexeme $ try $ do
  sgn <- option "" (string "-" <|> string "+")
  body <-
    try (do _ <- string "0x" <|> string "0X"; ds <- many1 (hexDigit <|> oneOf "'_"); pure ("0x" ++ ds))
      <|> many1 (digit <|> oneOf "'_")
  pure (filter (`notElem` "'_") (if sgn == "+" then body else sgn ++ body))

value :: Parser String
value = number <|> ident

------------------------------------------------------------------------------
-- Grammar

specification :: Parser [Def]
specification = concat <$> many item
  where
    item =
      (do kw "namespace"; _ <- ident; _ <- sym "{"; ds <- specification; _ <- sym "}"; pure ds)
        <|> ((: []) <$> definition)

definition :: Parser Def
definition =
  typedefDef <|> enumDef <|> structDef <|> unionDef <|> constDef <|> programDef
  where
    typedefDef = do kw "typedef"; d <- declaration; _ <- sym ";"; pure (DefTypedef d)
    enumDef = do kw "enum"; n <- ident; b <- enumBody; _ <- sym ";"; pure (DefEnum n b)
    structDef = do kw "struct"; n <- ident; b <- structBody; _ <- sym ";"; pure (DefStruct n b)
    unionDef = do kw "union"; n <- ident; b <- unionBody; _ <- sym ";"; pure (DefUnion n b)
    constDef = do kw "const"; n <- ident; _ <- sym "="; v <- number; _ <- sym ";"; pure (DefConst n v)

programDef :: Parser Def
programDef = do
  kw "program"
  n <- ident
  _ <- sym "{"
  vs <- many1 versDef
  _ <- sym "}"
  _ <- sym "="
  v <- value
  _ <- sym ";"
  pure (DefProgram n v vs)
  where
    versDef = do
      kw "version"
      n <- ident
      _ <- sym "{"
      ps <- many1 procDef
      _ <- sym "}"
      _ <- sym "="
      v <- value
      _ <- sym ";"
      pure (Vers n v ps)
    procDef = do
      ret <- (Nothing <$ kw "void") <|> (Just <$> scalarType)
      n <- ident
      _ <- sym "("
      args <- ([] <$ kw "void") <|> (scalarType `sepBy1` sym ",")
      _ <- sym ")"
      _ <- sym "="
      v <- value
      _ <- sym ";"
      pure (Proc n ret args v)

enumBody :: Parser [(String, String)]
enumBody = do
  _ <- sym "{"
  tags <- tag `sepEndBy1` sym ","
  _ <- sym "}"
  pure tags
  where
    tag = do n <- ident; _ <- sym "="; v <- value; pure (n, v)

structBody :: Parser [Decl]
structBody = do
  _ <- sym "{"
  ds <- many1 (voidDeclaration <* sym ";")
  _ <- sym "}"
  pure ds

unionBody :: Parser UnionBody
unionBody = do
  kw "switch"
  _ <- sym "("
  t <- scalarType
  i <- ident
  _ <- sym ")"
  _ <- sym "{"
  arms <- many1 arm
  _ <- sym "}"
  pure (UnionBody t i arms)
  where
    arm = defaultArm <|> caseArm
    defaultArm = do
      kw "default"
      _ <- sym ":"
      d <- voidDeclaration
      _ <- sym ";"
      pure (Arm [] True d)
    caseArm = do
      cs <- many1 (try (kw "case" >> value <* sym ":"))
      isDef <- option False (True <$ (kw "default" >> sym ":"))
      d <- voidDeclaration
      _ <- sym ";"
      pure (Arm cs isDef d)

voidDeclaration :: Parser Decl
voidDeclaration = (DVoid <$ kw "void") <|> declaration

declaration :: Parser Decl
declaration = opaqueDecl <|> stringDecl <|> typedDecl
  where
    opaqueDecl = do
      kw "opaque"
      n <- ident
      q <- (QArray <$> between (sym "[") (sym "]") value) <|> (QVec <$> between (sym "<") (sym ">") (optionMaybe value))
      pure (Decl BOpaque n q)
    stringDecl = do
      kw "string"
      n <- ident
      b <- between (sym "<") (sym ">") (optionMaybe value)
      pure (Decl BString n (QVec b))
    typedDecl = do
      t <- typeSpecifier
      ptr <- option False (True <$ sym "*")
      n <- ident
      if ptr
        then pure (Decl t n QPtr)
        else do
          q <-
            option
              QPlain
              ( (QArray <$> between (sym "[") (sym "]") value)
                  <|> (QVec <$> between (sym "<") (sym ">") (optionMaybe value))
              )
          pure (Decl t n q)

scalarType :: Parser Base
scalarType =
  (kw "unsigned" >> ((BUHyper <$ kw "hyper") <|> (BUInt <$ kw "int") <|> pure BUInt))
    <|> (BInt <$ kw "int")
    <|> (BHyper <$ kw "hyper")
    <|> (BBool <$ kw "bool")
    <|> (BFloat <$ kw "float")
    <|> (BDouble <$ kw "double")
    <|> (BNamed <$> ident)

typeSpecifier :: Parser Base
typeSpecifier =
  (kw "enum" >> BEnum <$> enumBody)
    <|> (kw "struct" >> BStruct <$> structBody)
    <|> (kw "union" >> BUnion <$> unionBody)
    <|> scalarType

parseFile :: FilePath -> IO [Def]
parseFile fp = do
  src <- readFile fp
  case parse (ws *> specification <* eof) fp src of
    Left e -> hPutStrLn stderr (show e) >> exitFailure
    Right ds -> pure ds

------------------------------------------------------------------------------
-- Hoisting anonymous nested types into named top-level definitions

-- | Replace inline enum/struct/union bases with named references.
hoist :: [Def] -> [Def]
hoist = concatMap goDef
  where
    goDef = \case
      DefTypedef d -> let (d', extra) = goDecl "" d in extra ++ [DefTypedef d']
      DefStruct n ds ->
        let (ds', extra) = unzip (map (goDecl n) ds) in concat extra ++ [DefStruct n ds']
      DefUnion n u -> let (u', extra) = goUnion n u in extra ++ [DefUnion n u']
      d -> [d]
    goUnion n (UnionBody t i arms) =
      let (arms', extra) = unzip [(a {armDecl = d'}, e) | a <- arms, let (d', e) = goDecl n (armDecl a)]
       in (UnionBody t i arms', concat extra)
    goDecl _ DVoid = (DVoid, [])
    goDecl parent d@(Decl b n q) =
      let nm = if null parent then n else parent ++ "_" ++ n ++ "_t"
       in case b of
            BEnum tags -> (Decl (BNamed nm) n q, [DefEnum nm tags])
            BStruct ds -> (Decl (BNamed nm) n q, goDef (DefStruct nm ds))
            BUnion u -> (Decl (BNamed nm) n q, goDef (DefUnion nm u))
            _ -> (d, [])

------------------------------------------------------------------------------
-- Global environment

data Kind = KStruct | KUnion | KEnum | KTypedef Decl
  deriving (Show)

data Env = Env
  { envKinds :: M.Map String Kind
  , envConsts :: M.Map String Integer
  -- ^ const and enum-tag values
  , envFileOf :: M.Map String String
  -- ^ type or const name -> module name
  , envRecursive :: S.Set String
  , envEnumTags :: M.Map String [(String, Integer)]
  -- ^ enum name -> resolved tags
  , envPatName :: M.Map (String, String) String
  -- ^ (enum, tag) -> Haskell pattern synonym name
  , envDefs :: M.Map String Def
  }

defName :: Def -> Maybe String
defName = \case
  DefTypedef (Decl _ n _) -> Just n
  DefEnum n _ -> Just n
  DefStruct n _ -> Just n
  DefUnion n _ -> Just n
  DefConst n _ -> Just n
  _ -> Nothing

parseNum :: String -> Maybe Integer
parseNum ('-' : s) = negate <$> parseNum s
parseNum ('0' : x : s) | x `elem` "xX" = Just (foldl (\a c -> a * 16 + fromIntegral (digitToInt c)) 0 s)
parseNum s | not (null s) && all isDigit s = Just (read s)
parseNum _ = Nothing

buildEnv :: [(String, [Def])] -> Env
buildEnv files = env
  where
    allDefs = [(m, d) | (m, ds) <- files, d <- ds]
    kinds =
      M.fromList $
        [(n, k) | (_, d) <- allDefs, Just (n, k) <- [kindOf d]]
    kindOf = \case
      DefTypedef d@(Decl _ n _) -> Just (n, KTypedef d)
      DefEnum n _ -> Just (n, KEnum)
      DefStruct n _ -> Just (n, KStruct)
      DefUnion n _ -> Just (n, KUnion)
      _ -> Nothing
    tagCounts = M.fromListWith (+) [(t, 1 :: Int) | (_, DefEnum _ tags) <- allDefs, (t, _) <- tags]
    uniqueTag t = M.lookup t tagCounts == Just 1
    rawConsts =
      [(n, v) | (_, DefConst n v) <- allDefs]
        ++ [(t, v) | (_, DefEnum _ tags) <- allDefs, (t, v) <- tags, uniqueTag t]
    consts = M.fromList [(n, resolve (10 :: Int) v) | (n, v) <- rawConsts]
    enumTags =
      M.fromList
        [ (n, [(t, resolveIn tags t v) | (t, v) <- tags])
        | (_, DefEnum n tags) <- allDefs
        ]
    resolveIn tags _ v = case parseNum v of
      Just x -> x
      Nothing -> case lookup v tags of
        Just v' -> resolveIn tags v v'
        Nothing -> resolve (10 :: Int) v
    typeCons = S.fromList ([hsType n | (_, DefStruct n _) <- allDefs] ++ [hsType n | (_, DefEnum n _) <- allDefs])
    patNames =
      M.fromList
        [ ((n, t), if uniqueTag t && not (hsCon t `S.member` typeCons) then hsCon t else hsType n ++ "_" ++ t)
        | (_, DefEnum n tags) <- allDefs
        , (t, _) <- tags
        ]
    rawMap = M.fromList rawConsts
    resolve 0 v = error ("cannot resolve constant " ++ v)
    resolve k v = case parseNum v of
      Just x -> x
      Nothing -> case M.lookup v rawMap of
        Just v' -> resolve (k - 1) v'
        Nothing -> error ("unknown constant " ++ v)
    fileOf =
      M.fromList $
        [(n, m) | (m, d) <- allDefs, Just n <- [defName d]]
          ++ [(t, m) | (m, DefEnum _ tags) <- allDefs, (t, _) <- tags]
    -- Direct type references of each struct/union/typedef.
    refs = M.fromList [(n, declRefs d) | (_, d) <- allDefs, Just n <- [defName d]]
    declRefs = \case
      DefTypedef d -> baseRefs d
      DefStruct _ ds -> concatMap baseRefs ds
      DefUnion _ u -> concatMap (baseRefs . armDecl) (uArms u)
      _ -> []
    baseRefs (Decl (BNamed t) _ _) = [t]
    baseRefs _ = []
    reach start = go S.empty (fromMaybe [] (M.lookup start refs))
      where
        go seen [] = seen
        go seen (x : xs)
          | x `S.member` seen = go seen xs
          | otherwise = go (S.insert x seen) (fromMaybe [] (M.lookup x refs) ++ xs)
    recursive =
      S.fromList
        [ n
        | (_, d) <- allDefs
        , Just n <- [defName d]
        , isSU d
        , n `S.member` reach n
        ]
    isSU = \case DefStruct _ _ -> True; DefUnion _ _ -> True; _ -> False
    env = Env kinds consts fileOf recursive enumTags patNames defsMap
    defsMap = M.fromList [(n, d) | (_, d) <- allDefs, Just n <- [defName d]]

------------------------------------------------------------------------------
-- Naming

reservedTypes :: S.Set String
reservedTypes =
  S.fromList
    [ "Int", "Int8", "Int16", "Int32", "Int64", "Word", "Word8", "Word16", "Word32", "Word64", "Bool", "Maybe"
    , "ByteString", "Builder", "Get", "Double", "Float", "Integer", "String", "Either", "Map", "Set", "IO"
    , "Char", "Ordering", "Show", "Eq", "Ord", "Text", "Maybe", "Monad", "Functor"
    ]

hsType :: String -> String
hsType n@(c : _)
  | isLower c || c == '_' = "X_" ++ n
  | n `S.member` reservedTypes = n ++ "_"
  | otherwise = n
hsType [] = error "empty type name"

hsKeywords :: S.Set String
hsKeywords =
  S.fromList
    [ "case", "class", "data", "default", "deriving", "do", "else", "foreign", "if", "import", "in", "infix"
    , "infixl", "infixr", "instance", "let", "module", "newtype", "of", "then", "type", "where", "_", "family"
    , "forall", "mdo", "rec", "proc", "pattern", "role", "qualified", "as", "hiding", "stock", "anyclass", "via"
    ]

hsField :: String -> String
hsField n@(c : _)
  | n `S.member` hsKeywords = n ++ "_"
  | isUpper c = "f_" ++ n
  | otherwise = n
hsField [] = error "empty field"

hsCon :: String -> String
hsCon (c : cs) = toUpper c : cs
hsCon [] = error "empty constructor"

hsConst :: String -> String
hsConst n@(c : _)
  | isUpper c = "c_" ++ n
  | n `S.member` hsKeywords = n ++ "_"
  | otherwise = n
hsConst [] = error "empty const"

caseLabel :: String -> String
caseLabel ('-' : s) = "m" ++ s
caseLabel s = s

moduleOfFile :: FilePath -> String
moduleOfFile fp = "Hogd.Xdr." ++ concatMap cap (splitOn '_' (takeBaseName fp))
  where
    cap (c : cs) = toUpper c : cs
    cap [] = []
    splitOn ch s = case break (== ch) s of
      (a, []) -> [a]
      (a, _ : rest) -> a : splitOn ch rest

------------------------------------------------------------------------------
-- Type mapping and codec expressions

hsBase :: Base -> String
hsBase = \case
  BInt -> "Int32"
  BUInt -> "Word32"
  BHyper -> "Int64"
  BUHyper -> "Word64"
  BBool -> "Bool"
  BFloat -> "Float"
  BDouble -> "Double"
  BOpaque -> "ByteString"
  BString -> "ByteString"
  BNamed n -> hsType n
  b -> error ("unhoisted base " ++ show b)

hsDeclType :: Decl -> String
hsDeclType DVoid = "()"
hsDeclType (Decl b _ q) = case (b, q) of
  (BOpaque, _) -> "ByteString"
  (BString, _) -> "ByteString"
  (_, QPlain) -> hsBase b
  (_, QPtr) -> "(Maybe " ++ hsBase b ++ ")"
  (_, QArray _) -> "[" ++ hsBase b ++ "]"
  (_, QVec _) -> "[" ++ hsBase b ++ "]"

-- | Codec function names for a base type with no qualifier.
data Codec = Codec {cPut :: String, cGet :: String, cCxsPut :: String, cCxsGet :: String}

baseCodec :: Env -> Base -> Codec
baseCodec env = \case
  BNamed n -> case M.lookup n (envKinds env) of
    Just (KTypedef _) ->
      let h = hsType n in Codec ("put_" ++ h) ("get_" ++ h) ("cxsPut_" ++ h) ("cxsGet_" ++ h)
    Just _ -> classCodec
    Nothing -> error ("unknown type " ++ n)
  BOpaque -> error "opaque without qualifier"
  BString -> error "string without qualifier"
  _ -> classCodec
  where
    classCodec = Codec "xdrPut" "xdrGet" "cxsPut" "cxsGet"

boundExpr :: Env -> Maybe String -> String
boundExpr _ Nothing = "0xffffffff"
boundExpr env (Just v) = show (constVal env v)

constVal :: Env -> String -> Integer
constVal env v = case parseNum v of
  Just x -> x
  Nothing -> fromMaybe (error ("unknown constant " ++ v)) (M.lookup v (envConsts env))

-- | (put, get, cxsPut, cxsGet) expressions for a declaration.
declCodec :: Env -> Decl -> Codec
declCodec _ DVoid = Codec "(const mempty)" "(pure ())" "(const mempty)" "(pure ())"
declCodec env (Decl b _ q) = case (b, q) of
  (BOpaque, QArray n) -> fixedOp (show (constVal env n))
  (BOpaque, QVec bnd) -> varOp (boundExpr env bnd)
  (BString, QVec bnd) -> varOp (boundExpr env bnd)
  (BOpaque, _) -> error "bad opaque"
  (BString, _) -> error "bad string"
  (_, QPlain) -> bc
  (_, QPtr) ->
    Codec
      ("(putOptional " ++ cPut bc ++ ")")
      ("(getOptional " ++ cGet bc ++ ")")
      ("(cxsPutOptional " ++ cCxsPut bc ++ ")")
      ("(cxsGetOptional " ++ cCxsGet bc ++ ")")
  (_, QArray n) ->
    let k = show (constVal env n)
     in Codec
          ("(putFixedArray " ++ k ++ " " ++ cPut bc ++ ")")
          ("(getFixedArray " ++ k ++ " " ++ cGet bc ++ ")")
          ("(cxsPutFixedArray " ++ k ++ " " ++ cCxsPut bc ++ ")")
          ("(cxsGetFixedArray " ++ k ++ " " ++ cCxsGet bc ++ ")")
  (_, QVec bnd) ->
    let k = boundExpr env bnd
     in Codec
          ("(putVarArray " ++ k ++ " " ++ cPut bc ++ ")")
          ("(getVarArray " ++ k ++ " " ++ cGet bc ++ ")")
          ("(cxsPutVarArray " ++ k ++ " " ++ cCxsPut bc ++ ")")
          ("(cxsGetVarArray " ++ k ++ " " ++ cCxsGet bc ++ ")")
  where
    bc = baseCodec env b
    fixedOp k =
      Codec ("(putFixedOpaque " ++ k ++ ")") ("(getFixedOpaque " ++ k ++ ")") ("(cxsPutFixedOpaque " ++ k ++ ")") ("(cxsGetFixedOpaque " ++ k ++ ")")
    varOp k =
      Codec ("(putVarOpaque " ++ k ++ ")") ("(getVarOpaque " ++ k ++ ")") ("(cxsPutVarOpaque " ++ k ++ ")") ("(cxsGetVarOpaque " ++ k ++ ")")

------------------------------------------------------------------------------
-- Code generation

data Out = Out {oLines :: [String], oRefs :: S.Set String}

emit :: [String] -> Out
emit ls = Out ls S.empty

instance Semigroup Out where
  Out a b <> Out c d = Out (a ++ c) (S.union b d)

instance Monoid Out where
  mempty = Out [] S.empty

-- | Names referenced by a declaration (for imports).
declNames :: Decl -> [String]
declNames (Decl (BNamed n) _ _) = [n]
declNames _ = []

deriv :: String
deriv = "  deriving (Eq, Ord, Show)"

genDef :: Env -> Def -> Out
genDef env d = genDef' env d <> genBounds env d

-- | Encoded-size bounds (x23's xdr_size_bounds) for non-recursive types.
genBounds :: Env -> Def -> Out
genBounds env d = case defName d of
  Just n | not (isConstDef d) -> case typeBounds env S.empty (BNamed n) of
    Just (lo, hi) ->
      emit
        [ "xdrMin_" ++ hsType n ++ ", xdrMax_" ++ hsType n ++ " :: Integer"
        , "xdrMin_" ++ hsType n ++ " = " ++ show lo
        , "xdrMax_" ++ hsType n ++ " = " ++ show hi
        , ""
        ]
    Nothing -> mempty
  _ -> mempty
  where
    isConstDef = \case DefConst _ _ -> True; _ -> False

pad4 :: Integer -> Integer
pad4 x = (x + 3) `div` 4 * 4

typeBounds :: Env -> S.Set String -> Base -> Maybe (Integer, Integer)
typeBounds env seen = \case
  BInt -> Just (4, 4)
  BUInt -> Just (4, 4)
  BBool -> Just (4, 4)
  BFloat -> Just (4, 4)
  BHyper -> Just (8, 8)
  BUHyper -> Just (8, 8)
  BDouble -> Just (8, 8)
  BNamed n
    | n `S.member` seen -> Nothing
    | otherwise -> case M.lookup n (envDefs env) of
        Just (DefEnum _ _) -> Just (4, 4)
        Just (DefTypedef dcl) -> declBounds env (S.insert n seen) dcl
        Just (DefStruct _ ds) -> do
          bs <- mapM (declBounds env (S.insert n seen)) ds
          pure (sum (map fst bs), sum (map snd bs))
        Just (DefUnion _ (UnionBody _ _ arms)) -> do
          bs <- mapM (declBounds env (S.insert n seen) . armDecl) arms
          pure (4 + minimum (map fst bs), 4 + maximum (map snd bs))
        _ -> Nothing
  _ -> Nothing

declBounds :: Env -> S.Set String -> Decl -> Maybe (Integer, Integer)
declBounds _ _ DVoid = Just (0, 0)
declBounds env seen (Decl b _ q) = case (b, q) of
  (BOpaque, QArray n) -> let k = pad4 (constVal env n) in Just (k, k)
  (BOpaque, QVec bnd) -> Just (4, 4 + pad4 (vecBound bnd))
  (BString, QVec bnd) -> Just (4, 4 + pad4 (vecBound bnd))
  (_, QPlain) -> typeBounds env seen b
  (_, QPtr) -> do (_, hi) <- typeBounds env seen b; pure (4, 4 + hi)
  (_, QArray n) -> do
    (lo, hi) <- typeBounds env seen b
    let k = constVal env n
    pure (k * lo, k * hi)
  (_, QVec bnd) -> do (_, hi) <- typeBounds env seen b; pure (4, 4 + vecBound bnd * hi)
  _ -> Nothing
  where
    vecBound = maybe 0xffffffff (constVal env)

genDef' :: Env -> Def -> Out
genDef' env = \case
  DefConst n v ->
    emit
      [ hsConst n ++ " :: Num a => a"
      , hsConst n ++ " = " ++ show (constVal env v)
      , ""
      ]
  DefEnum n _ -> genEnum env n
  DefTypedef d -> genTypedef env d
  DefStruct n ds -> genStruct env n ds
  DefUnion n u -> genUnion env n u
  DefProgram n v vs -> genProgram env n v vs

genEnum :: Env -> String -> Out
genEnum env n =
  emit $
    [ "newtype " ++ t ++ " = " ++ t ++ " Int32"
    , "  deriving (Eq, Ord)"
    , ""
    ]
      ++ concat
        [ [ "pattern " ++ pn tg ++ " :: " ++ t
          , "pattern " ++ pn tg ++ " = " ++ t ++ " (" ++ show v ++ ")"
          ]
        | (tg, v) <- tags'
        ]
      ++ [ ""
         , "instance Show " ++ t ++ " where"
         , "  showsPrec d (" ++ t ++ " v) = case v of"
         ]
      ++ [ "    " ++ paren v ++ " -> showString " ++ show (pn tg)
         | (tg, v) <- nubOn snd tags'
         ]
      ++ [ "    _ -> showParen (d > 10) (showString " ++ show (t ++ " ") ++ " . showsPrec 11 v)"
         , ""
         , "instance XdrType " ++ t ++ " where"
         , "  xdrPut (" ++ t ++ " v) = putInt32 v"
         , "  xdrGet = " ++ t ++ " <$> getInt32"
         , "  cxsPut (" ++ t ++ " v) = cxsPutInt32 v"
         , "  cxsGet = " ++ t ++ " <$> cxsGetInt32"
         , ""
         , "-- | Every declared value of '" ++ t ++ "'."
         , lowerFirst t ++ "_values :: [" ++ t ++ "]"
         , lowerFirst t ++ "_values = [" ++ intercalate ", " [pn tg | (tg, _) <- tags'] ++ "]"
         , ""
         ]
  where
    t = hsType n
    tags' = fromMaybe [] (M.lookup n (envEnumTags env))
    pn tg = fromMaybe (error "pattern name") (M.lookup (n, tg) (envPatName env))
    paren v = if v < 0 then "(" ++ show v ++ ")" else show v

nubOn :: Ord b => (a -> b) -> [a] -> [a]
nubOn f = go S.empty
  where
    go _ [] = []
    go s (x : xs)
      | f x `S.member` s = go s xs
      | otherwise = x : go (S.insert (f x) s) xs

lowerFirst :: String -> String
lowerFirst (c : cs) = toLower c : cs
lowerFirst [] = []

genTypedef :: Env -> Decl -> Out
genTypedef _ DVoid = error "void typedef"
genTypedef env d@(Decl _ n _) =
  Out
    [ "type " ++ h ++ " = " ++ stripParens (hsDeclType d)
    , ""
    , "put_" ++ h ++ " :: " ++ h ++ " -> Builder"
    , "put_" ++ h ++ " = " ++ cPut c
    , ""
    , "get_" ++ h ++ " :: Get " ++ h
    , "get_" ++ h ++ " = " ++ cGet c
    , ""
    , "cxsPut_" ++ h ++ " :: " ++ h ++ " -> Builder"
    , "cxsPut_" ++ h ++ " = " ++ cCxsPut c
    , ""
    , "cxsGet_" ++ h ++ " :: Get " ++ h
    , "cxsGet_" ++ h ++ " = " ++ cCxsGet c
    , ""
    ]
    (S.fromList (declNames d))
  where
    h = hsType n
    c = declCodec env d

stripParens :: String -> String
stripParens s
  | "(" `isPrefixOf` s && last s == ')' = init (tail s)
  | otherwise = s

recWrap :: Env -> String -> String -> String
recWrap env n g
  | n `S.member` envRecursive env = "getRecursive (" ++ g ++ ")"
  | otherwise = g

genStruct :: Env -> String -> [Decl] -> Out
genStruct env n ds =
  Out
    ( [ "data " ++ t ++ " = " ++ t
      ]
        ++ ( if null fields
              then []
              else
                ["  { " ++ intercalate "\n  , " [hsField f ++ " :: " ++ hsDeclType d | (f, d) <- fields] ++ "\n  }"]
           )
        ++ [ deriv
           , ""
           , "instance XdrType " ++ t ++ " where"
           , "  xdrPut " ++ pat ++ " = " ++ joinPuts [cPut (declCodec env d) ++ " " ++ v | ((_, d), v) <- zip fields vars]
           , "  xdrGet = " ++ recWrap env n (getExpr cGet)
           , "  cxsPut " ++ pat ++ " = " ++ joinPuts [cCxsPut (declCodec env d) ++ " " ++ v | ((_, d), v) <- zip fields vars]
           , "  cxsGet = " ++ recWrap env n (getExpr cCxsGet)
           , ""
           ]
    )
    (S.fromList (concatMap declNames ds))
  where
    t = hsType n
    fields = [(f, d) | d@(Decl _ f _) <- ds]
    vars = ["x" ++ show i | i <- [1 .. length fields]]
    pat = if null fields then t else "(" ++ t ++ " " ++ unwords vars ++ ")"
    getExpr sel
      | null fields = "pure " ++ t
      | otherwise = t ++ " <$> " ++ intercalate " <*> " [sel (declCodec env d) | (_, d) <- fields]

joinPuts :: [String] -> String
joinPuts [] = "mempty"
joinPuts xs = intercalate " <> " xs

-- | A constructor plan for one union arm.
data ArmPlan = ArmPlan
  { apCon :: String
  , apCases :: [Integer]
  , apDefault :: Bool
  , apCarriesDisc :: Bool
  , apDecl :: Decl
  }

discType :: Env -> Base -> (String, String, String, String, String)
-- (haskell type, put, get, cxsPut, cxsGet)
discType env = \case
  BInt -> ("Int32", "putInt32", "getInt32", "cxsPutInt32", "cxsGetInt32")
  BUInt -> ("Word32", "putWord32", "getWord32", "cxsPutWord32", "cxsGetWord32")
  BBool -> ("Bool", "putBool", "getBool", "cxsPutBool", "cxsGetBool")
  BNamed n -> case M.lookup n (envKinds env) of
    Just KEnum -> (hsType n, "xdrPut", "xdrGet", "cxsPut", "cxsGet")
    Just (KTypedef (Decl b _ QPlain)) -> discType env b
    _ -> error ("bad discriminant type " ++ n)
  b -> error ("bad discriminant " ++ show b)

isEnumDisc :: Env -> Base -> Maybe String
isEnumDisc env = \case
  BNamed n -> case M.lookup n (envKinds env) of
    Just KEnum -> Just (hsType n)
    Just (KTypedef (Decl b _ QPlain)) -> isEnumDisc env b
    _ -> Nothing
  _ -> Nothing

caseValue :: Env -> Base -> String -> Integer
caseValue _ _ "TRUE" = 1
caseValue _ _ "FALSE" = 0
caseValue env b v = case enumNameOf env b >>= \e -> M.lookup e (envEnumTags env) >>= lookup v of
  Just x -> x
  Nothing -> constVal env v

enumNameOf :: Env -> Base -> Maybe String
enumNameOf env = \case
  BNamed n -> case M.lookup n (envKinds env) of
    Just KEnum -> Just n
    Just (KTypedef (Decl b _ QPlain)) -> enumNameOf env b
    _ -> Nothing
  _ -> Nothing

discLit :: Env -> Base -> Integer -> String
discLit env b v = case isEnumDisc env b of
  Just t -> "(" ++ t ++ " (" ++ show v ++ "))"
  Nothing -> case b of
    BBool -> if v == 0 then "False" else "True"
    _ -> "(" ++ show v ++ ")"

genUnion :: Env -> String -> UnionBody -> Out
genUnion env n (UnionBody swb _ arms) =
  Out
    ( [ "data " ++ t
      , "  = " ++ intercalate "\n  | " (map conDecl plans)
      , deriv
      , ""
      , "instance XdrType " ++ t ++ " where"
      ]
        ++ concatMap (putClause "xdrPut" dPut cPut) plans
        ++ ["  xdrGet = " ++ recWrap env n ("do\n    d <- " ++ dGet ++ "\n    case d of\n" ++ getCases dGet cGet)]
        ++ concatMap (putClause "cxsPut" dCxsPut cCxsPut) plans
        ++ ["  cxsGet = " ++ recWrap env n ("do\n    d <- " ++ dCxsGet ++ "\n    case d of\n" ++ getCases dCxsGet cCxsGet)]
        ++ [ ""
           , "-- | The discriminant of a '" ++ t ++ "'."
           , "discriminant_" ++ t ++ " :: " ++ t ++ " -> " ++ dT
           ]
        ++ concatMap discClause plans
        ++ [""]
    )
    (S.fromList (concatMap (declNames . armDecl) arms ++ discNames))
  where
    t = hsType n
    discNames = case swb of BNamed x -> [x]; _ -> []
    (dT, dPut, dGet, dCxsPut, dCxsGet) = discType env swb
    plans = dedupe (map plan arms)
    plan (Arm cs isDef d) =
      let vals = map (caseValue env swb) cs
          base = case d of
            Decl _ f _ -> f
            DVoid
              | isDef && null cs -> "default"
              | otherwise -> caseLabel (head cs)
          carries = isDef || length cs > 1
       in ArmPlan (t ++ "_" ++ base) vals isDef carries d
    dedupe ps = snd (foldl step (S.empty, []) ps)
      where
        step (seen, acc) p =
          let c = head [c' | k <- [0 :: Int ..], let c' = if k == 0 then apCon p else apCon p ++ show k, not (c' `S.member` seen)]
           in (S.insert c seen, acc ++ [p {apCon = c}])
    conDecl p =
      apCon p
        ++ (if apCarriesDisc p then " " ++ wrapT dT else "")
        ++ (case apDecl p of DVoid -> ""; d -> " " ++ wrapT (hsDeclType d))
    wrapT s = if ' ' `elem` s && not ("(" `isPrefixOf` s) && not ("[" `isPrefixOf` s) then "(" ++ s ++ ")" else s
    lit = discLit env swb
    putClause nm dp sel p =
      let c = sel (declCodec env (apDecl p))
          hasVal = case apDecl p of DVoid -> False; _ -> True
       in case (apCarriesDisc p, hasVal) of
            (False, False) -> ["  " ++ nm ++ " " ++ apCon p ++ " = " ++ dp ++ " " ++ lit (head (apCases p))]
            (False, True) -> ["  " ++ nm ++ " (" ++ apCon p ++ " v) = " ++ dp ++ " " ++ lit (head (apCases p)) ++ " <> " ++ c ++ " v"]
            (True, _) ->
              [ "  " ++ nm ++ " (" ++ apCon p ++ " d" ++ (if hasVal then " v" else "") ++ ")"
              , "    | " ++ validGuard p ++ " = " ++ dp ++ " d" ++ (if hasVal then " <> " ++ c ++ " v" else "")
              , "    | otherwise = error " ++ show ("bad discriminant for " ++ apCon p)
              ]
    explicitVals = concatMap apCases plans
    validGuard p
      | apDefault p && null (apCases p) = "d `notElem` [" ++ intercalate ", " (map lit explicitVals) ++ "]"
      | apDefault p = "True"
      | otherwise = "d `elem` [" ++ intercalate ", " (map lit (apCases p)) ++ "]"
    getCases _ sel =
      concat
        [ "      " ++ lit v ++ " -> " ++ armGet sel p ++ "\n"
        | p <- plans
        , v <- apCases p
        ]
        ++ case [p | p <- plans, apDefault p] of
          (p : _) -> "      _ -> " ++ armGet sel p
          [] -> "      _ -> getFail " ++ show ("bad discriminant deserializing " ++ n)
    armGet sel p =
      let c = sel (declCodec env (apDecl p))
       in case (apCarriesDisc p, apDecl p) of
            (False, DVoid) -> "pure " ++ apCon p
            (False, _) -> apCon p ++ " <$> " ++ c
            (True, DVoid) -> "pure (" ++ apCon p ++ " d)"
            (True, _) -> apCon p ++ " d <$> " ++ c
    discClause p = case (apCarriesDisc p, apDecl p) of
      (False, DVoid) -> ["discriminant_" ++ t ++ " " ++ apCon p ++ " = " ++ lit (head (apCases p))]
      (False, _) -> ["discriminant_" ++ t ++ " (" ++ apCon p ++ " _) = " ++ lit (head (apCases p))]
      (True, DVoid) -> ["discriminant_" ++ t ++ " (" ++ apCon p ++ " d) = d"]
      (True, _) -> ["discriminant_" ++ t ++ " (" ++ apCon p ++ " d _) = d"]

genProgram :: Env -> String -> String -> [Vers] -> Out
genProgram env pn pv vs =
  Out
    ( [ lowerFirst (hsType pn) ++ "_program :: Word32"
      , lowerFirst (hsType pn) ++ "_program = " ++ show (constVal env pv)
      , ""
      ]
        ++ concatMap genVers vs
    )
    (S.fromList (concat [mapMaybe namedOf (maybeToList (pRet p) ++ pArgs p) | v <- vs, p <- vProcs v]))
  where
    namedOf (BNamed x) = Just x
    namedOf _ = Nothing
    genVers (Vers vn vnum procs) =
      let r = hsType vn
          verNum = show (constVal env vnum)
          retT p = maybe "()" hsBase (pRet p)
          argT p = concat [hsBase a ++ " -> " | a <- pArgs p]
          bc b = baseCodec env b
       in [ "-- | Server implementation of " ++ pn ++ " version " ++ vn ++ "."
          , "data " ++ r ++ " = " ++ r
          , "  { " ++ intercalate "\n  , " [hsField (pName p) ++ " :: " ++ argT p ++ "IO " ++ retT p | p <- procs] ++ "\n  }"
          , ""
          , lowerFirst r ++ "_version :: Word32"
          , lowerFirst r ++ "_version = " ++ verNum
          , ""
          , "-- | Dispatch table for '" ++ r ++ "'."
          , lowerFirst r ++ "_service :: " ++ r ++ " -> RpcService"
          , lowerFirst r ++ "_service impl = RpcService " ++ lowerFirst (hsType pn) ++ "_program " ++ verNum ++ " dispatch"
          , "  where"
          , "    dispatch proc = case proc of"
          ]
            ++ concat
              [ [ "      " ++ show (constVal env (pNum p)) ++ " -> Just $ do"
                ]
                  ++ [ "        a" ++ show i ++ " <- " ++ cGet (bc a)
                     | (i, a) <- zip [1 :: Int ..] (pArgs p)
                     ]
                  ++ [ "        pure $ do"
                     , "          r <- impl." ++ hsField (pName p) ++ concat [" a" ++ show i | i <- [1 .. length (pArgs p)]]
                     , "          pure $! " ++ maybe "const mempty" (cPut . bc) (pRet p) ++ " r"
                     ]
              | p <- procs
              ]
            ++ [ "      _ -> Nothing"
               , ""
               ]
            ++ concat
              [ [ "call_" ++ pName p ++ " :: RpcClient -> " ++ argT p ++ "IO " ++ retT p
                , "call_" ++ pName p ++ " c" ++ concat [" a" ++ show i | i <- [1 .. length (pArgs p)]]
                    ++ " = rpcCall c "
                    ++ lowerFirst (hsType pn)
                    ++ "_program "
                    ++ verNum
                    ++ " "
                    ++ show (constVal env (pNum p))
                    ++ " ("
                    ++ joinPuts [cPut (bc a) ++ " a" ++ show i | (i, a) <- zip [1 :: Int ..] (pArgs p)]
                    ++ ") "
                    ++ maybe "(pure ())" (cGet . bc) (pRet p)
                , ""
                ]
              | p <- procs
              ]

------------------------------------------------------------------------------
-- Module assembly

header :: String -> [String] -> Bool -> [String]
header modName imports hasProgram =
  [ "{-# LANGUAGE DuplicateRecordFields #-}"
  , "{-# LANGUAGE NoFieldSelectors #-}"
  , "{-# LANGUAGE OverloadedRecordDot #-}"
  , "{-# LANGUAGE PatternSynonyms #-}"
  , "{-# OPTIONS_GHC -Wno-unused-imports -Wno-name-shadowing -Wno-overlapping-patterns -Wno-incomplete-patterns -Wno-missing-pattern-synonym-signatures #-}"
  , ""
  , "-- | Generated by xdrgen.  Do not edit."
  , "module " ++ modName ++ " where"
  , ""
  , "import Data.ByteString (ByteString)"
  , "import Data.Int"
  , "import Data.Word"
  , "import X23.Xdr"
  ]
    ++ ["import X23.Rpc (RpcService (..), RpcClient, rpcCall)" | hasProgram]
    ++ ["import " ++ i | i <- imports]
    ++ [""]

main :: IO ()
main = do
  setLocaleEncoding utf8
  args <- getArgs
  case args of
    (outDir : files@(_ : _)) -> run outDir files
    _ -> hPutStrLn stderr "usage: xdrgen OUTDIR FILE.x..." >> exitFailure

run :: FilePath -> [FilePath] -> IO ()
run outDir files = do
  parsed <- forM files $ \f -> do
    ds <- parseFile f
    pure (moduleOfFile f, hoist ds)
  let env = buildEnv parsed
  checkCollisions parsed
  createDirectoryIfMissing True (outDir </> "Hogd" </> "Xdr")
  forM_ parsed $ \(m, ds) -> do
    let out = mconcat (map (genDef env) ds)
        refMods =
          S.toList $
            S.fromList
              [ fm
              | r <- S.toList (oRefs out)
              , Just fm <- [M.lookup r (envFileOf env)]
              , fm /= m
              ]
        hasProg = any (\case DefProgram {} -> True; _ -> False) ds
        path = outDir </> joinPath (splitOn '.' m) <.> "hs"
    writeFile path (unlines (header m refMods hasProg ++ oLines out))
  -- Umbrella module.
  writeFile
    (outDir </> "Hogd" </> "Xdr.hs")
    ( unlines $
        ["-- | Generated by xdrgen.  Re-exports every protocol module.", "module Hogd.Xdr (" ]
          ++ ["  module " ++ m ++ "," | (m, _) <- parsed]
          ++ ["  ) where", ""]
          ++ ["import " ++ m | (m, _) <- parsed]
    )
  where
    splitOn ch s = case break (== ch) s of
      (a, []) -> [a]
      (a, _ : rest) -> a : splitOn ch rest

-- | Reject inputs whose Haskell names would collide.
checkCollisions :: [(String, [Def])] -> IO ()
checkCollisions parsed = do
  let defs = concatMap snd parsed
      typeNames = [hsType n | d <- defs, Just n <- [defName d], not (isConst d)]
      isConst = \case DefConst _ _ -> True; _ -> False
      conNames =
        [hsType n | DefStruct n _ <- defs]
          ++ [hsType n | DefEnum n _ <- defs]
      dupes xs = [x | (x, c) <- M.toList (M.fromListWith (+) [(x, 1 :: Int) | x <- xs]), c > 1]
  let bad = dupes typeNames ++ dupes conNames
  unless (null bad) $ do
    hPutStrLn stderr ("name collisions: " ++ show (nub bad))
    exitFailure
  -- Sort for determinism in error output.
  void (pure (sortOn fst parsed))
