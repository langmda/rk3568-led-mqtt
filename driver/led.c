#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/poll.h>
struct led_priv
{
    struct gpio_desc *led_gpio;
    struct gpio_desc *key_gpio;
    int irq;
    dev_t dev;
    struct cdev mycdev;
    struct class *myclass;
    struct device *mydevice;
    struct delayed_work button_work;
    struct mutex lock;
    wait_queue_head_t button_wq; // 按键事件等待队列
    atomic_t flag;               // 按键事件标志

    atomic_t removed;            // 移除标志位
    atomic_t open_count;         // 文件引用计数
    wait_queue_head_t remove_wq; // 卸载等待队列，等待open_count归零
};

static int led_open(struct inode *inode, struct file *file)
{
    struct led_priv *priv = container_of(inode->i_cdev, struct led_priv, mycdev);

    /* 已移除直接拒绝 */
    if (atomic_read(&priv->removed))
        return -ENODEV;

    /* 先加引用，再复查 removed，已被移除就减少引用*/
    atomic_inc(&priv->open_count);

    if (atomic_read(&priv->removed))
    {
        if (atomic_dec_and_test(&priv->open_count))		//将引用计数减一。如果结果为零，就返回TRUE，如果不为零，就返回FLASE
            wake_up(&priv->remove_wq);					//如果将引用减为0，就唤醒卸载等待队列
        return -ENODEV;
    }

    file->private_data = priv;
    printk(KERN_INFO "Open success\n");
    return 0;
}

static int led_release(struct inode *inode, struct file *file)
{
    struct led_priv *priv = file->private_data;

    if (!priv)
        return 0;

    if (atomic_dec_and_test(&priv->open_count))
        wake_up(&priv->remove_wq);   

    return 0;
}

static ssize_t led_read(struct file *file, char __user *buf,
                        size_t size, loff_t *off)
{
    struct led_priv *priv = file->private_data;
    int ret;
    char kbuf[2];

    if (size < 1)
        return -EINVAL;

    while (1)
    {
        if (atomic_read(&priv->removed))		/* 已移除，不要再碰 GPIO */
            return -ENODEV;

        if (atomic_xchg(&priv->flag, 0))		//将flag替换成0，并返回旧值
            break;

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        ret = wait_event_interruptible(priv->button_wq,
                                       atomic_read(&priv->flag) ||
                                       atomic_read(&priv->removed));		
        if (ret < 0)			//不是满足条件被唤醒，而是被信号唤醒
            return ret;
    }

    kbuf[0] = '1';
    kbuf[1] = '\0';

    if (copy_to_user(buf, kbuf, 1))
        return -EFAULT;

    return 1;
}

static ssize_t led_write(struct file *file, const char __user *buf,
                         size_t size, loff_t *off)
{
    struct led_priv *priv = file->private_data;
    char kbuf[2];
    int ret;

    if (size < 1)
        return -EINVAL;
    
    if (atomic_read(&priv->removed))	/* 已移除，不要再碰 GPIO */
        return -ENODEV;

    ret = copy_from_user(kbuf, buf, 1);
    if (ret)
        return -EFAULT;

    mutex_lock(&priv->lock);

    /* 拿锁后再复查一次，避免 remove 在 copy_from_user 期间完成 */
    if (atomic_read(&priv->removed))
    {
        mutex_unlock(&priv->lock);
        return -ENODEV;
    }

    if (kbuf[0] == '0')
        gpiod_set_value_cansleep(priv->led_gpio, 0);
    else if (kbuf[0] == '1')
        gpiod_set_value_cansleep(priv->led_gpio, 1);

    mutex_unlock(&priv->lock);
    return size;
}
static __poll_t led_poll(struct file *file, struct poll_table_struct *wait)
{
      struct led_priv *priv = file->private_data;
      __poll_t mask = 0;

      poll_wait(file, &priv->button_wq, wait);

      // 检查模块是否已卸载
      if (atomic_read(&priv->removed))
      {
            mask |= POLLERR | POLLHUP; // 设备、文件发生错误，对端挂断
            return mask;
      }

      // 检查是否可读
      if (atomic_read(&priv->flag))
      {
            mask |= POLLIN | POLLRDNORM; // 可读，普通数据可读
      }

      return mask;
}
static struct file_operations fops =
    {
        .owner = THIS_MODULE,
        .open = led_open,
        .write = led_write,
        .read = led_read,
        .poll = led_poll,
};
static void button_work_handler(struct work_struct *work)
{
      struct led_priv *priv = container_of(work, struct led_priv, button_work.work);
      int val;
      mutex_lock(&priv->lock);
      val = gpiod_get_value_cansleep(priv->key_gpio);
      if (val) // val=1时（由于设备树key-gpio是GPIO_ACTIVE_LOW低电平有效，所以按键按下GPIO接地时，val=1）
      {
            int led = gpiod_get_value_cansleep(priv->led_gpio);
            gpiod_set_value_cansleep(priv->led_gpio, !led);
      }
      mutex_unlock(&priv->lock);
      if (val) // 放在锁之外，防止增加复杂度
      {
            atomic_set(&priv->flag, 1);
            wake_up_interruptible(&priv->button_wq);
      }
}
static irqreturn_t led_interrupt_handler(int irq, void *dev_id)
{
      struct led_priv *priv = dev_id;

      mod_delayed_work(system_wq, &priv->button_work, msecs_to_jiffies(20));
      return IRQ_HANDLED;
}
static int led_probe(struct platform_device *pdev)
{
      int ret;

      struct led_priv *priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
      if (!priv)
            return -ENOMEM;

      mutex_init(&priv->lock);

      atomic_set(&priv->removed, 0);			//加入初始化
      atomic_set(&priv->open_count, 0);          
      init_waitqueue_head(&priv->remove_wq);   
      init_waitqueue_head(&priv->button_wq);

      priv->led_gpio = gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
      if (IS_ERR(priv->led_gpio))
      {
            printk(KERN_INFO "Get led_gpio error\n");
            ret = PTR_ERR(priv->led_gpio);
            return ret;
      }
      gpiod_direction_output(priv->led_gpio, 0);

      priv->key_gpio = gpiod_get(&pdev->dev, "key", GPIOD_IN);
      if (IS_ERR(priv->key_gpio))
      {
            printk(KERN_INFO "Get key_gpio error\n");
            gpiod_put(priv->led_gpio);
            ret = PTR_ERR(priv->key_gpio);
            return ret;
      }
      priv->irq = gpiod_to_irq(priv->key_gpio);
      if (priv->irq < 0)
      {
            printk(KERN_INFO "To irq error\n");
            gpiod_put(priv->key_gpio);
            gpiod_put(priv->led_gpio);
            return -1;
      }

      INIT_DELAYED_WORK(&priv->button_work, button_work_handler);

      ret = request_irq(priv->irq, led_interrupt_handler, IRQF_TRIGGER_FALLING, "led_interrupt", priv);
      if (ret < 0)
      {
            printk(KERN_INFO "Request irq error\n");
            cancel_delayed_work_sync(&priv->button_work);
            gpiod_put(priv->key_gpio);
            gpiod_put(priv->led_gpio);
            return -1;
      }

      ret = alloc_chrdev_region(&priv->dev, 0, 1, "myled");
      if (ret < 0)
      {
            printk(KERN_INFO "Alloc dev error\n");
            free_irq(priv->irq, priv);
            cancel_delayed_work_sync(&priv->button_work);
            gpiod_put(priv->key_gpio);
            gpiod_put(priv->led_gpio);
            return -1;
      }
      cdev_init(&priv->mycdev, &fops);
      ret = cdev_add(&priv->mycdev, priv->dev, 1);
      if (ret < 0)
      {
            printk(KERN_INFO "Add cdev error\n");
            free_irq(priv->irq, priv);
            cancel_delayed_work_sync(&priv->button_work);
            unregister_chrdev_region(priv->dev, 1);
            gpiod_put(priv->key_gpio);
            gpiod_put(priv->led_gpio);
            return -1;
      }

      priv->myclass = class_create(THIS_MODULE, "myled");
      if (IS_ERR(priv->myclass))
      {
            printk(KERN_INFO "Class create error\n");
            free_irq(priv->irq, priv);
            cancel_delayed_work_sync(&priv->button_work);
            ret = PTR_ERR(priv->myclass);
            cdev_del(&priv->mycdev);
            unregister_chrdev_region(priv->dev, 1);
            gpiod_put(priv->key_gpio);
            gpiod_put(priv->led_gpio);
            return ret;
      }

      priv->mydevice = device_create(priv->myclass, NULL, priv->dev, NULL, "myled");
      if (IS_ERR(priv->mydevice))
      {
            printk(KERN_INFO "Device create error\n");
            free_irq(priv->irq, priv);
            cancel_delayed_work_sync(&priv->button_work);
            ret = PTR_ERR(priv->mydevice);
            class_destroy(priv->myclass);
            cdev_del(&priv->mycdev);
            unregister_chrdev_region(priv->dev, 1);
            gpiod_put(priv->key_gpio);
            gpiod_put(priv->led_gpio);
            return ret;
      }

      platform_set_drvdata(pdev, priv);

      return 0;
}
static int led_remove(struct platform_device *pdev)
{
      struct led_priv *priv = platform_get_drvdata(pdev);

      atomic_set(&priv->removed, 1); // 标记模块已卸载
      atomic_set(&priv->flag, 1);
      wake_up_interruptible(&priv->button_wq);

      wait_event(priv->remove_wq, atomic_read(&priv->open_count) == 0);
      
      /* 1. 先切断中断和延时工作队列，保证后面不再通过按键操作 GPIO */
      free_irq(priv->irq, priv);
      cancel_delayed_work_sync(&priv->button_work);

      /* 2. 关灯、注销设备节点，确保不会再通过系统调用操作GPIO（前面已经设置removed标志位并等待引用计数归零，所以其实不会有系统调用可以操作GPIO了）*/
	  //1和2顺序可换，只要3在最后就行
      gpiod_set_value_cansleep(priv->led_gpio, 0);
      device_destroy(priv->myclass, priv->dev);
      class_destroy(priv->myclass);
      cdev_del(&priv->mycdev);
      unregister_chrdev_region(priv->dev, 1);

      /* 3. 最后释放 GPIO */
      gpiod_put(priv->key_gpio);
      gpiod_put(priv->led_gpio);
      return 0;
}
static struct of_device_id led_of_match[] =
    {
        {.compatible = "topeet,myled"},
        {}};
MODULE_DEVICE_TABLE(of, led_of_match);
static struct platform_driver my_platform_driver =
    {
        .driver =
            {
                .name = "my_led_device",
                .owner = THIS_MODULE,
                .of_match_table = led_of_match,
            },
        .probe = led_probe,
        .remove = led_remove,
};
static int __init led_init(void)
{
      int ret;
      ret = platform_driver_register(&my_platform_driver);
      if (ret < 0)
      {
            printk(KERN_INFO "Platform register error\n");
            return -1;
      }
      return 0;
}
static void __exit led_exit(void)
{
      platform_driver_unregister(&my_platform_driver);
}
module_init(led_init);
module_exit(led_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("topeet");