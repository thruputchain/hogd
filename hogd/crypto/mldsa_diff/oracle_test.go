package mldsadiff

import (
	"bufio"
	"crypto/mldsa"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"os/exec"
)

// oracle is a running mldsa_oracle, which answers for hogd. Its source,
// ../mldsa_oracle.cc, documents the protocol.
type oracle struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	w      *bufio.Writer
	r      *bufio.Reader
	exited bool
}

func startOracle(path string) (*oracle, error) {
	cmd := exec.Command(path)
	cmd.Stderr = os.Stderr
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return &oracle{
		cmd:   cmd,
		stdin: stdin,
		w:     bufio.NewWriter(stdin),
		r:     bufio.NewReader(stdout),
	}, nil
}

// close ends the oracle's input and waits for it to exit.
func (o *oracle) close() error {
	if o.exited {
		return nil
	}
	o.exited = true
	o.stdin.Close()
	return o.cmd.Wait()
}

// failed explains err, an error talking to the oracle, with how the oracle
// exited if it did, e.g. on a crash.
func (o *oracle) failed(err error) error {
	if werr := o.close(); werr != nil {
		return fmt.Errorf("mldsa_oracle: %v (%v)", werr, err)
	}
	return fmt.Errorf("mldsa_oracle: %v", err)
}

func (o *oracle) writeField(b []byte) {
	o.w.Write(binary.LittleEndian.AppendUint32(nil, uint32(len(b))))
	o.w.Write(b)
}

// flag reads a 0 or 1 response.
func (o *oracle) flag() (bool, error) {
	if err := o.w.Flush(); err != nil {
		return false, o.failed(err)
	}
	b, err := o.r.ReadByte()
	if err != nil {
		return false, o.failed(err)
	}
	if b > 1 {
		return false, o.failed(fmt.Errorf("response %d is not 0 or 1", b))
	}
	return b == 1, nil
}

// publicKey makes the oracle derive the key pair for seed, which it then
// signs with, and returns the public key.
func (o *oracle) publicKey(seed []byte) ([]byte, error) {
	o.w.WriteByte('K')
	o.w.Write(seed)
	if err := o.w.Flush(); err != nil {
		return nil, o.failed(err)
	}
	pk := make([]byte, mldsa.MLDSA65PublicKeySize)
	if _, err := io.ReadFull(o.r, pk); err != nil {
		return nil, o.failed(err)
	}
	return pk, nil
}

// sign signs msg under ctx with the key from the last publicKey call, using
// rnd as the signing randomness. It returns nil if hogd refuses to sign.
func (o *oracle) sign(rnd, ctx, msg []byte) ([]byte, error) {
	o.w.WriteByte('S')
	o.w.Write(rnd)
	o.writeField(ctx)
	o.writeField(msg)
	signed, err := o.flag()
	if err != nil || !signed {
		return nil, err
	}
	sig := make([]byte, mldsa.MLDSA65SignatureSize)
	if _, err := io.ReadFull(o.r, sig); err != nil {
		return nil, o.failed(err)
	}
	return sig, nil
}

// verify reports whether hogd's mldsa65_verify accepts sig.
func (o *oracle) verify(pk, sig, ctx, msg []byte) (bool, error) {
	o.w.WriteByte('V')
	o.w.Write(pk)
	o.w.Write(sig)
	o.writeField(ctx)
	o.writeField(msg)
	return o.flag()
}
