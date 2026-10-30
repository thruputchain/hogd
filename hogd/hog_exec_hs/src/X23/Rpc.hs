{-# LANGUAGE LambdaCase #-}

-- | ONC RPC (RFC 5531) over TCP with record marking, wire-compatible with
-- x23's RpcStream server and client.
--
-- Servers dispatch each call on its own thread and may reply out of order;
-- clients multiplex concurrent calls over one connection by xid.
module X23.Rpc
  ( -- * Services
    RpcService (..)
  , RpcServer
  , rpcServerPort
  , bindRpcServer
  , startAccepting
  , stopRpcServer
  , defaultMaxMessageSize
    -- * Clients
  , RpcClient
  , RpcException (..)
  , connectRpcClient
  , connectRpcClientRetry
  , connectRpcClientWith
  , closeRpcClient
  , rpcCall
    -- * Addresses
  , resolveHostPort
  , parseHostPort
  ) where

import Control.Concurrent
import Control.Concurrent.Async (async, cancel)
import Control.Exception
import Control.Monad
import Data.Bits
import qualified Data.ByteString as B
import Data.ByteString.Builder (Builder)
import qualified Data.ByteString.Builder as BB
import qualified Data.ByteString.Lazy as BL
import Data.IORef
import qualified Data.IntMap.Strict as IM
import qualified Data.Map.Strict as M
import Data.Word
import Network.Socket
import qualified Network.Socket.ByteString as NSB
import qualified Network.Socket.ByteString.Lazy as NSBL
import Text.Read (readMaybe)
import X23.Xdr

-- | One program version's procedures.  @dispatch proc@ returns Nothing for an
-- unknown procedure; otherwise a decoder for the arguments that yields the
-- action computing the encoded result.
data RpcService = RpcService
  { svcProg :: !Word32
  , svcVers :: !Word32
  , svcDispatch :: Word32 -> Maybe (Get (IO Builder))
  }

-- | x23::default_max_message_size.
defaultMaxMessageSize :: Int
defaultMaxMessageSize = 0x1000000

------------------------------------------------------------------------------
-- Record marking

recvExactly :: Socket -> Int -> IO (Maybe B.ByteString)
recvExactly sock n = go [] n
  where
    go acc 0 = pure (Just (B.concat (reverse acc)))
    go acc k = do
      chunk <- NSB.recv sock (min k 1048576)
      if B.null chunk
        then pure Nothing
        else go (chunk : acc) (k - B.length chunk)

-- | Read one record (possibly several fragments).  Nothing on EOF; throws
-- on an oversized record.
readRecord :: Int -> Socket -> IO (Maybe B.ByteString)
readRecord maxSize sock = go [] 0
  where
    go acc total =
      recvExactly sock 4 >>= \case
        Nothing -> pure Nothing
        Just hdr -> do
          let w = B.foldl' (\a b -> (a `shiftL` 8) .|. fromIntegral b) (0 :: Word32) hdr
              lastFrag = testBit w 31
              len = fromIntegral (w .&. 0x7fffffff)
              total' = total + len
          when (total' > maxSize) $ throwIO (RpcProtocolError ("record of " ++ show total' ++ " bytes exceeds limit"))
          body <- if len == 0 then pure (Just B.empty) else recvExactly sock len
          case body of
            Nothing -> pure Nothing
            Just b
              | lastFrag -> pure (Just (B.concat (reverse (b : acc))))
              | otherwise -> go (b : acc) total'

-- | Frame a message as a single record.
makeRecord :: Builder -> BL.ByteString
makeRecord msg =
  let body = BB.toLazyByteString msg
      len = BL.length body
   in BB.toLazyByteString (BB.word32BE (0x80000000 .|. fromIntegral len)) <> body

------------------------------------------------------------------------------
-- Message headers

data RpcException
  = RpcConnectionClosed
  | RpcProtocolError String
  | RpcDenied String
  | RpcAcceptError Word32
  -- ^ accept_stat other than SUCCESS
  | RpcDecodeError String
  | RpcMessageTooLarge Int
  deriving (Show)

instance Exception RpcException

data CallHeader = CallHeader
  { chXid :: !Word32
  , chProg :: !Word32
  , chVers :: !Word32
  , chProc :: !Word32
  }

getOpaqueAuth :: Get ()
getOpaqueAuth = do
  _ <- getInt32
  _ <- getVarOpaque 400
  pure ()

putNullAuth :: Builder
putNullAuth = putInt32 0 <> putWord32 0

-- | Parse the call header; on failure return the xid if it was readable.
getCallHeader :: Get CallHeader
getCallHeader = do
  xid <- getWord32
  mtype <- getWord32
  unless (mtype == 0) $ getFail "not a call"
  rpcvers <- getWord32
  unless (rpcvers == 2) $ getFail "bad rpc version"
  prog <- getWord32
  vers <- getWord32
  proc' <- getWord32
  getOpaqueAuth
  getOpaqueAuth
  pure (CallHeader xid prog vers proc')

acceptedReply :: Word32 -> Word32 -> Builder -> Builder
acceptedReply xid stat body =
  putWord32 xid <> putWord32 1 <> putWord32 0 <> putNullAuth <> putWord32 stat <> body

------------------------------------------------------------------------------
-- Server

data RpcServer = RpcServer
  { rsSocket :: Socket
  , rsPort :: Word16
  , rsAccepter :: IORef (Maybe (IO ()))
  }

rpcServerPort :: RpcServer -> Word16
rpcServerPort = rsPort

-- | Bind and listen, without accepting yet (connections queue in the
-- backlog), so the caller can learn the port before serving.
bindRpcServer :: String -> Word16 -> IO RpcServer
bindRpcServer host port = do
  addr <- resolveHostPort host port
  sock <- socket (addrFamily addr) Stream defaultProtocol
  setSocketOption sock ReuseAddr 1
  bind sock (addrAddress addr)
  listen sock 1024
  actual <- socketPort sock
  ref <- newIORef Nothing
  pure (RpcServer sock (fromIntegral actual) ref)

-- | Start serving the given services on a background thread.
startAccepting :: RpcServer -> Int -> [RpcService] -> IO ()
startAccepting srv maxMsg services = do
  let table = M.fromListWith (++) [(svcProg s, [s]) | s <- services]
  a <- async $ forever $ do
    (conn, _) <- accept (rsSocket srv)
    setSocketOption conn NoDelay 1
    void $ forkFinally (serveConnection maxMsg table conn) (\_ -> close conn)
  writeIORef (rsAccepter srv) (Just (cancel a))

stopRpcServer :: RpcServer -> IO ()
stopRpcServer srv = do
  readIORef (rsAccepter srv) >>= sequence_
  close (rsSocket srv)

serveConnection :: Int -> M.Map Word32 [RpcService] -> Socket -> IO ()
serveConnection maxMsg table conn = do
  wlock <- newMVar ()
  let send b = withMVar wlock $ \_ -> NSBL.sendAll conn (makeRecord b)
      loop =
        readRecord maxMsg conn >>= \case
          Nothing -> pure ()
          Just msg -> do
            handleCall send msg
            loop
  loop
  where
    handleCall send msg = case runGet deserializationRecursionLimit getCallHeader msg of
      Left _ -> pure () -- unparseable: drop, as x23 does for garbage
      Right (CallHeader xid prog vers proc', off) -> do
        let args = B.drop off msg
        case M.lookup prog table of
          Nothing -> send (acceptedReply xid 1 mempty) -- PROG_UNAVAIL
          Just svcs -> case [s | s <- svcs, svcVers s == vers] of
            [] ->
              let vs = map svcVers svcs
               in send (acceptedReply xid 2 (putWord32 (minimum vs) <> putWord32 (maximum vs)))
            (s : _) -> case svcDispatch s proc' of
              Nothing -> send (acceptedReply xid 3 mempty) -- PROC_UNAVAIL
              Just decoder -> case runGet deserializationRecursionLimit decoder args of
                Right (action, n)
                  | n == B.length args ->
                      void $ forkIO $ do
                        r <- try (action >>= evaluateBuilder)
                        case r of
                          Right body -> send (acceptedReply xid 0 (BB.lazyByteString body))
                          Left e -> do
                            logRpcFailure (e :: SomeException)
                            send (acceptedReply xid 5 mempty) -- SYSTEM_ERR
                _ -> send (acceptedReply xid 4 mempty) -- GARBAGE_ARGS
    evaluateBuilder b = do
      let lbs = BB.toLazyByteString b
      _ <- evaluate (BL.length lbs)
      pure lbs

logRpcFailure :: SomeException -> IO ()
logRpcFailure e = case fromException e of
  Just ThreadKilled -> pure ()
  _ -> putStrLn ("rpc handler failed: " ++ show e)

------------------------------------------------------------------------------
-- Client

data RpcClient = RpcClient
  { rcSocket :: Socket
  , rcWriteLock :: MVar ()
  , rcNextXid :: IORef Word32
  , rcPending :: MVar (Maybe (IM.IntMap (MVar (Either RpcException B.ByteString))))
  -- ^ Nothing once the connection has failed
  , rcMaxMessage :: Int
  , rcReader :: IORef (Maybe ThreadId)
  }

connectRpcClient :: String -> Word16 -> IO RpcClient
connectRpcClient = connectRpcClientWith False defaultMaxMessageSize

-- | Connect, retrying while the peer refuses connections.
connectRpcClientRetry :: String -> Word16 -> IO RpcClient
connectRpcClientRetry = connectRpcClientWith True defaultMaxMessageSize

-- | Connect with an explicit retry policy and message-size cap (which bounds
-- both requests sent and replies accepted).
connectRpcClientWith :: Bool -> Int -> String -> Word16 -> IO RpcClient
connectRpcClientWith retry maxMsg host port = do
  addr <- resolveHostPort host port
  let attempt = do
        sock <- socket (addrFamily addr) Stream defaultProtocol
        r <- try (connect sock (addrAddress addr))
        case r of
          Right () -> pure sock
          Left (e :: IOException)
            | retry -> close sock >> threadDelay 100000 >> attempt
            | otherwise -> close sock >> throwIO e
  sock <- attempt
  setSocketOption sock NoDelay 1
  wl <- newMVar ()
  xidRef <- newIORef 1
  pending <- newMVar (Just IM.empty)
  readerRef <- newIORef Nothing
  let client = RpcClient sock wl xidRef pending maxMsg readerRef
  tid <- forkIO (clientReader client)
  writeIORef readerRef (Just tid)
  pure client

closeRpcClient :: RpcClient -> IO ()
closeRpcClient c = do
  readIORef (rcReader c) >>= mapM_ killThread
  failAll c RpcConnectionClosed
  close (rcSocket c)

failAll :: RpcClient -> RpcException -> IO ()
failAll c e = do
  old <- modifyMVar (rcPending c) (\m -> pure (Nothing, m))
  forM_ old $ \m -> forM_ (IM.elems m) $ \v -> tryPutMVar v (Left e)

clientReader :: RpcClient -> IO ()
clientReader c = do
  r <- try loop
  case r of
    Left (e :: SomeException) -> failAll c (RpcProtocolError (show e))
    Right () -> failAll c RpcConnectionClosed
  where
    loop =
      readRecord (rcMaxMessage c) (rcSocket c) >>= \case
        Nothing -> pure ()
        Just msg -> do
          case runGet deserializationRecursionLimit getWord32 msg of
            Right (xid, _) -> do
              slot <- modifyMVar (rcPending c) $ \case
                Nothing -> pure (Nothing, Nothing)
                Just m -> pure (Just (IM.delete (fromIntegral xid) m), IM.lookup (fromIntegral xid) m)
              forM_ slot $ \v -> tryPutMVar v (parseReply msg)
            Left _ -> pure ()
          loop

-- | Parse a reply message, returning the result bytes on success.
parseReply :: B.ByteString -> Either RpcException B.ByteString
parseReply msg = case runGet deserializationRecursionLimit hdr msg of
  Left e -> Left (RpcProtocolError e)
  Right (Left err, _) -> Left err
  Right (Right (), off) -> Right (B.drop off msg)
  where
    hdr = do
      _xid <- getWord32
      mtype <- getWord32
      unless (mtype == 1) $ getFail "not a reply"
      stat <- getWord32
      case stat of
        0 -> do
          getOpaqueAuth
          acc <- getWord32
          pure (if acc == 0 then Right () else Left (RpcAcceptError acc))
        _ -> do
          rej <- getWord32
          pure (Left (RpcDenied ("rejected: " ++ show rej)))

-- | Perform one call.  Throws 'RpcException' on transport or protocol
-- failure.
rpcCall :: RpcClient -> Word32 -> Word32 -> Word32 -> Builder -> Get a -> IO a
rpcCall c prog vers proc' args decoder = do
  xid <- atomicModifyIORef' (rcNextXid c) (\x -> (x + 1, x))
  slot <- newEmptyMVar
  ok <- modifyMVar (rcPending c) $ \case
    Nothing -> pure (Nothing, False)
    Just m -> pure (Just (IM.insert (fromIntegral xid) slot m), True)
  unless ok $ throwIO RpcConnectionClosed
  let msg =
        putWord32 xid <> putWord32 0 <> putWord32 2 <> putWord32 prog <> putWord32 vers <> putWord32 proc'
          <> putNullAuth
          <> putNullAuth
          <> args
      record = makeRecord msg
  when (fromIntegral (BL.length record) - 4 > rcMaxMessage c) $ do
    _ <- modifyMVar_ (rcPending c) (pure . fmap (IM.delete (fromIntegral xid)))
    throwIO (RpcMessageTooLarge (fromIntegral (BL.length record) - 4))
  sent <- try (withMVar (rcWriteLock c) $ \_ -> NSBL.sendAll (rcSocket c) record)
  case sent of
    Left (e :: IOException) -> do
      failAll c (RpcProtocolError (show e))
      throwIO RpcConnectionClosed
    Right () -> pure ()
  takeMVar slot >>= \case
    Left e -> throwIO e
    Right body -> case runGet deserializationRecursionLimit decoder body of
      Left e -> throwIO (RpcDecodeError e)
      Right (a, n)
        | n /= B.length body -> throwIO (RpcDecodeError "leftover bytes in reply")
        | otherwise -> pure a

------------------------------------------------------------------------------
-- Addresses

resolveHostPort :: String -> Word16 -> IO AddrInfo
resolveHostPort host port = do
  let hints = defaultHints {addrSocketType = Stream, addrFlags = [AI_NUMERICSERV]}
      host' = if null host then Nothing else Just (stripBrackets host)
  addrs <- getAddrInfo (Just hints) host' (Just (show port))
  case addrs of
    (a : _) -> pure a
    [] -> throwIO (userError ("cannot resolve " ++ host))
  where
    stripBrackets ('[' : rest) | not (null rest) && last rest == ']' = init rest
    stripBrackets h = h

-- | Parse "host:port" or "[v6]:port".
parseHostPort :: String -> Maybe (String, Word16)
parseHostPort s = case break (== ':') (reverse s) of
  (rport, ':' : rhost)
    | not (null rhost) -> do
        p <- readMaybe (reverse rport) :: Maybe Integer
        guard (p >= 0 && p <= 65535)
        let h = reverse rhost
            h' = case h of
              '[' : rest | not (null rest) && last rest == ']' -> init rest
              _ -> h
        pure (h', fromIntegral p)
  _ -> Nothing
