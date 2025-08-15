namespace hogd {

enum ShardResult {
  ShardOK = 0,
};

program Shard
{
  version Shard1
  {

    void shard_null(void) = 0;

    // shard_restore blocks until restoration completes.
    ShardResult shard_restore(BlockHeight) = 10;
  }
  = 1;
}
= 0x7ee70101;

} // namespace hogd
