#ifndef __CASTLE_CTRL_H__
#define __CASTLE_CTRL_H__

int castle_control_ioctl(struct inode *inode, struct file *filp,
                         unsigned int cmd, unsigned long arg);
int castle_ctrl_packet_process(struct sk_buff *skb, void *reply, int *len_p);
int castle_control_init(void);
void castle_control_fini(void);

#endif /* __CASTLE_CTRL_H__ */
