#include <vector>
#include <stdexcept>

template<typename T>
class CircularBuffer
{
public:
    class iterator
    {
    public:
        using iterator_category=std::forward_iterator_tag;
        using difference_type=std::ptrdiff_t;
        using value_type=T;
        using pointer=T*;
        using reference=T&;

    public:
        iterator(CircularBuffer* buffer,size_t pos)
        :buffer_(buffer),pos_(pos)
        {}

        reference operator*()
        {
            return buffer_->buffer_[pos_];
        }
        
        pointer operator->()
        {
            return &buffer_->buffer_[pos_];
        }      

        iterator& operator++()
        {
            pos_=buffer_->next(pos_);
            if(pos_==buffer_->tail_||buffer_->empty())
            {
                buffer_=nullptr;
                pos_=0;
            }  
            return *this;
        }

        iterator operator++(int)
        {
            iterator temp=*this;
            ++(*this);
            return temp;
        }

        friend bool operator==(const iterator& lhs,const iterator& rhs) 
        {
            return lhs.buffer_==rhs.buffer_&&lhs.pos_==rhs.pos_;
        };
        
        friend bool operator!=(const iterator& lhs,const iterator& rhs) 
        {
            return !(lhs==rhs);
        };

    private:
        CircularBuffer* buffer_;
        size_t pos_;
    };

public:
    explicit CircularBuffer(size_t capacity) 
    :capacity_(capacity),size_(0),head_(0),tail_(0)
    {
        buffer_.resize(capacity_);
    }

    void push_back(const T& item) 
    {
        if(size_==capacity_) 
        {
            pop_front();
            buffer_[tail_]=item;
            tail_=next(tail_);
        }
        else 
        {
            buffer_[tail_]=item;
            tail_=next(tail_);
            ++size_;
        }
    }

    void pop_front() 
    {
        if(empty())  throw std::underflow_error("Buffer is empty");
        head_=next(head_);
        --size_;
    }

    T& front() 
    {
        if(empty())  throw std::underflow_error("Buffer is empty");
        return buffer_[head_];
    }

    T& back() 
    {
        if(empty())  throw std::underflow_error("Buffer is empty");
        return buffer_[prev(tail_)];
    }

    bool empty()const 
    {
        return size_==0;
    }

    size_t size()const 
    {
        return size_;
    }

    size_t capacity()const 
    {
        return capacity_;
    }

    iterator begin() 
    {
        if(empty())  return end();
        return iterator{this,head_};
    }

    iterator end() 
    {
        return iterator{nullptr,0};
    }

private:
    size_t next(size_t index)const
    {
        return (index+1)%capacity_;
    }

    size_t prev(size_t index)const
    {
        return (index-1+capacity_)%capacity_;
    }

private:
    std::vector<T> buffer_;
    size_t capacity_;
    size_t size_;
    size_t head_;
    size_t tail_;
}; 