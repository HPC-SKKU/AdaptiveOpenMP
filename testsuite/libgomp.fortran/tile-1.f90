module matrix
  implicit none
  integer :: n = 10
  integer :: m =  10

contains
  function mult (a, b) result (c)
    integer, allocatable, dimension (:,:) :: a,b,c
    integer :: i, j, k, inner

    allocate(c( n, m ))
    !$omp parallel do collapse(2) private(inner, i, j, k)
    !$omp tile sizes (8, 1)
    do i = 1,m
      do j = 1,n
        inner = 0
        do k = 1, n
          inner = inner + a(k, i) * b(j, k)
        end do
        c(j, i) = inner
      end do
    end do
  end function mult

  subroutine print_matrix (m)
    integer, allocatable :: m(:,:)
    integer :: i, j, n

    n = size (m, 1)
    do i = 1,n
      do j = 1,n
        write (*, fmt="(i4)", advance='no') m(j, i)
      end do
      write (*, *)  ""
    end do
    write (*, *)  ""
  end subroutine

end module matrix

program main
  use matrix
  implicit none

  integer, allocatable :: a(:,:),b(:,:),c(:,:)
  integer :: i,j

  allocate(a( n, m ))
  allocate(b( n, m ))

  do i = 1,n
    do j = 1,m
      a(j,i) = merge(1,0, i.eq.j)
      b(j,i) = j
    end do
  end do

  c = mult (a, b)

  call print_matrix (a)
  call print_matrix (b)
  call print_matrix (c)

  do i = 1,n
    do j = 1,m
      if (b(i,j) .ne. c(i,j)) stop 1
    end do
  end do

end program main
